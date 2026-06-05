/*
 * edgevib_video.c — EdgeVib Virtual V4L2 Video Capture Device
 *
 * Creates one or more virtual V4L2 video capture devices (/dev/videoX)
 * backed by VMALLOC single-buffer vb2_queue. A Go daemon injects JPEG
 * frames decoded to YUYV via a cdev interface.
 *
 * Architecture:
 *   edgevib-video-d (Go daemon): TimescaleDB vision_captures poll
 *     → JPEG decode + YCbCr→YUYV → write to /dev/edgevib-video-inject
 *   edgevib_video.ko: cdev write → vmalloc frame buffer → vb2_queue
 *     → /dev/videoX (V4L2 capture device)
 *
 * Multi-device (D5 HWMON pattern):
 *   num_devices= module param → N × video_device_register()
 *   /dev/video0 = "EdgeVib Virtual Cam (dev0)", etc.
 *
 * Standard V4L2 ioctls:
 *   QUERYCAP, ENUM_FMT, ENUM_FRAMESIZES, G_FMT, S_FMT, TRY_FMT
 *   REQBUFS, QUERYBUF, QBUF, DQBUF, STREAMON, STREAMOFF
 *
 * Custom sysfs (on cdev device):
 *   /sys/devices/virtual/edgevib-video-inject/frame_count
 *   /sys/devices/virtual/edgevib-video-inject/last_frame_time_ms
 *
 * Usage:
 *   insmod edgevib_video.ko num_devices=2
 *   v4l2-ctl --list-devices | grep "EdgeVib Virtual Cam"
 *   v4l2-ctl -d /dev/video0 --list-formats
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/version.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-vmalloc.h>

#define EDGEVIB_VIDEO_DEV_NAME    "edgevib-video"
#define EDGEVIB_INJECT_DEV_NAME   "edgevib-video-inject"
#define EDGEVIB_MAX_DEVICES       8
#define EDGEVIB_DEFAULT_DEVICES   2
#define EDGEVIB_INJECT_HEADER_SZ  20  /* 5 × s32 */

/* ---- Module parameter (D5 HWMON pattern) ---- */

static int num_devices = EDGEVIB_DEFAULT_DEVICES;
module_param(num_devices, int, 0444);
MODULE_PARM_DESC(num_devices, "Number of virtual V4L2 devices (1-8, default 2)");

/* ---- Data structures ---- */

/* Per-device state — one per /dev/videoX */
struct edgevib_video_dev {
	int                     id;             /* 0..num_devices-1 */
	char                    name[48];       /* "EdgeVib Virtual Cam (dev0)" */
	bool                    active;

	/* V4L2 core */
	struct video_device    *vdev;
	struct v4l2_device      v4l2_dev;
	struct vb2_queue        vb2_q;
	struct mutex            lock;           /* protects frame_data + streaming + queued_buf */

	/* Current frame (vmalloc'd) */
	u8                     *frame_data;
	s32                     width;
	s32                     height;
	u32                     pixelformat;    /* V4L2_PIX_FMT_YUYV */

	/* Streaming state (D6 single-event model) */
	bool                    streaming;
	struct vb2_v4l2_buffer *queued_buf;     /* single buffer */
};

/* Binary injection struct header — userspace writes this to cdev
 * [device_id:4B][width:4B][height:4B][pixelformat:4B][data_size:4B][data:N]
 */
struct video_inject_header {
	s32 device_id;
	s32 width;
	s32 height;
	u32 pixelformat;
	s32 data_size;
};

/* Top-level driver private data (D5 g_priv pattern) */
struct edgevib_video_priv {
	struct edgevib_video_dev *devices;
	int                       num_devices;

	/* cdev for data injection */
	dev_t                     cdev_num;
	struct cdev               cdev;
	struct class             *cdev_class;
	struct device            *cdev_device;

	/* Global sysfs counters */
	atomic_t                  frame_count;
	atomic64_t                last_frame_time_ms;
};

static struct edgevib_video_priv *g_priv;

/* ---- Forward declarations ---- */

/* ─── vb2_ops callbacks ─── */

static int edgevib_queue_setup(struct vb2_queue *q,
			       unsigned int *num_buffers,
			       unsigned int *num_planes,
			       unsigned int sizes[],
			       struct device *alloc_devs[])
{
	struct edgevib_video_dev *dev = vb2_get_drv_priv(q);

	/* Single buffer, single plane (D6 single-event model) */
	if (*num_buffers == 0)
		*num_buffers = 1;
	if (*num_buffers > 1)
		*num_buffers = 1;
	*num_planes = 1;

	sizes[0] = dev->width * dev->height * 2;  /* YUYV = 2 bytes/pixel */
	return 0;
}

static void edgevib_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_queue *q = vb->vb2_queue;
	struct edgevib_video_dev *dev = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	mutex_lock(&dev->lock);
	dev->queued_buf = vbuf;
	mutex_unlock(&dev->lock);
}

static int edgevib_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct edgevib_video_dev *dev = vb2_get_drv_priv(q);

	mutex_lock(&dev->lock);
	dev->streaming = true;
	mutex_unlock(&dev->lock);
	return 0;
}

static void edgevib_stop_streaming(struct vb2_queue *q)
{
	struct edgevib_video_dev *dev = vb2_get_drv_priv(q);

	mutex_lock(&dev->lock);
	if (dev->queued_buf) {
		struct vb2_buffer *vb = &dev->queued_buf->vb2_buf;
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		dev->queued_buf = NULL;
	}
	dev->streaming = false;
	mutex_unlock(&dev->lock);
}

static void edgevib_wait_prepare(struct vb2_queue *q)
{
	struct edgevib_video_dev *dev = vb2_get_drv_priv(q);
	mutex_unlock(&dev->lock);
}

static void edgevib_wait_finish(struct vb2_queue *q)
{
	struct edgevib_video_dev *dev = vb2_get_drv_priv(q);
	mutex_lock(&dev->lock);
}

static const struct vb2_ops edgevib_vb2_ops = {
	.queue_setup     = edgevib_queue_setup,
	.buf_queue       = edgevib_buf_queue,
	.start_streaming = edgevib_start_streaming,
	.stop_streaming  = edgevib_stop_streaming,
	.wait_prepare    = edgevib_wait_prepare,
	.wait_finish     = edgevib_wait_finish,
};

/* ─── v4l2_file_operations ─── */

static int edgevib_video_open(struct file *filp)
{
	struct video_device *vdev = video_devdata(filp);
	struct edgevib_video_dev *dev = video_get_drvdata(vdev);

	if (!dev->active)
		return -ENODEV;

	/* Call standard v4l2_fh_open — handles fh init + queue association */
	return v4l2_fh_open(filp);
}

static int edgevib_video_release(struct file *filp)
{
	/* Standard vb2 release — handles queued buffers + fh cleanup */
	return vb2_fop_release(filp);
}

static const struct v4l2_file_operations edgevib_video_fops = {
	.owner          = THIS_MODULE,
	.open           = edgevib_video_open,
	.release        = edgevib_video_release,
	.unlocked_ioctl = video_ioctl2,
	.mmap           = vb2_fop_mmap,
	.poll           = vb2_fop_poll,
	.read           = vb2_fop_read,
};

/* ─── v4l2_ioctl_ops ─── */

static int edgevib_querycap(struct file *filp, void *fh,
			    struct v4l2_capability *cap)
{
	struct video_device *vdev = video_devdata(filp);
	struct edgevib_video_dev *dev = video_get_drvdata(vdev);

	strscpy(cap->driver, "edgevib_video", sizeof(cap->driver));
	strscpy(cap->card, dev->name, sizeof(cap->card));
	strscpy(cap->bus_info, "virtual", sizeof(cap->bus_info));
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int edgevib_enum_fmt(struct file *filp, void *fh,
			     struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_YUYV;
	strscpy(f->description, "YUYV 4:2:2", sizeof(f->description));
	return 0;
}

static int edgevib_enum_framesizes(struct file *filp, void *fh,
				    struct v4l2_frmsizeenum *f)
{
	if (f->index > 1 || f->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	f->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	switch (f->index) {
	case 0:
		f->discrete.width  = 640;
		f->discrete.height = 480;
		break;
	case 1:
		f->discrete.width  = 1920;
		f->discrete.height = 1080;
		break;
	}
	return 0;
}

static int edgevib_g_fmt(struct file *filp, void *fh,
			  struct v4l2_format *f)
{
	struct video_device *vdev = video_devdata(filp);
	struct edgevib_video_dev *dev = video_get_drvdata(vdev);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	mutex_lock(&dev->lock);
	f->fmt.pix.width        = dev->width;
	f->fmt.pix.height       = dev->height;
	f->fmt.pix.pixelformat  = dev->pixelformat;
	f->fmt.pix.field        = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = dev->width * 2;         /* YUYV = 2 bytes/px */
	f->fmt.pix.sizeimage    = dev->width * dev->height * 2;
	f->fmt.pix.colorspace   = V4L2_COLORSPACE_SRGB;
	mutex_unlock(&dev->lock);
	return 0;
}

static int edgevib_try_fmt(struct file *filp, void *fh,
			    struct v4l2_format *f)
{
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (f->fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	/* Accept 640x480 or 1920x1080, clamp others to nearest */
	if (f->fmt.pix.width <= 640) {
		f->fmt.pix.width  = 640;
		f->fmt.pix.height = 480;
	} else {
		f->fmt.pix.width  = 1920;
		f->fmt.pix.height = 1080;
	}

	f->fmt.pix.bytesperline = f->fmt.pix.width * 2;
	f->fmt.pix.sizeimage    = f->fmt.pix.width * f->fmt.pix.height * 2;
	f->fmt.pix.field        = V4L2_FIELD_NONE;
	f->fmt.pix.colorspace   = V4L2_COLORSPACE_SRGB;
	return 0;
}

static int edgevib_s_fmt(struct file *filp, void *fh,
			  struct v4l2_format *f)
{
	struct video_device *vdev = video_devdata(filp);
	struct edgevib_video_dev *dev = video_get_drvdata(vdev);
	int ret;

	ret = edgevib_try_fmt(filp, fh, f);
	if (ret)
		return ret;

	mutex_lock(&dev->lock);
	dev->width       = f->fmt.pix.width;
	dev->height      = f->fmt.pix.height;
	dev->pixelformat = f->fmt.pix.pixelformat;
	mutex_unlock(&dev->lock);
	return 0;
}

static const struct v4l2_ioctl_ops edgevib_ioctl_ops = {
	.vidioc_querycap          = edgevib_querycap,
	.vidioc_enum_fmt_vid_cap  = edgevib_enum_fmt,
	.vidioc_enum_framesizes   = edgevib_enum_framesizes,
	.vidioc_g_fmt_vid_cap     = edgevib_g_fmt,
	.vidioc_s_fmt_vid_cap     = edgevib_s_fmt,
	.vidioc_try_fmt_vid_cap   = edgevib_try_fmt,
	.vidioc_reqbufs           = vb2_ioctl_reqbufs,
	.vidioc_querybuf          = vb2_ioctl_querybuf,
	.vidioc_qbuf              = vb2_ioctl_qbuf,
	.vidioc_dqbuf             = vb2_ioctl_dqbuf,
	.vidioc_streamon          = vb2_ioctl_streamon,
	.vidioc_streamoff         = vb2_ioctl_streamoff,
	.vidioc_subscribe_event   = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ─── cdev file_operations (injection interface) ─── */

static int edgevib_inject_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int edgevib_inject_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t edgevib_inject_write(struct file *filp,
				    const char __user *buf,
				    size_t count, loff_t *pos)
{
	struct edgevib_video_priv *priv;
	struct edgevib_video_dev *dev;
	struct video_inject_header hdr;
	u8 *new_frame = NULL;
	size_t expected_size;
	int ret;

	if (!g_priv)
		return -ENODEV;
	priv = g_priv;

	/* 1. Read 20-byte fixed header */
	if (count < EDGEVIB_INJECT_HEADER_SZ)
		return -EINVAL;

	ret = copy_from_user(&hdr, buf, EDGEVIB_INJECT_HEADER_SZ);
	if (ret)
		return -EFAULT;

	/* 2. Validate device_id */
	if (hdr.device_id < 0 || hdr.device_id >= priv->num_devices)
		return -EINVAL;
	dev = &priv->devices[hdr.device_id];
	if (!dev->active)
		return -ENODEV;

	/* 3. Validate pixel format */
	if (hdr.pixelformat != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	/* 4. Validate dimensions and data size */
	if (hdr.width <= 0 || hdr.height <= 0)
		return -EINVAL;
	expected_size = hdr.width * hdr.height * 2;  /* YUYV = 2 bytes/pixel */
	if (hdr.data_size != expected_size)
		return -EINVAL;

	/* 5. Validate total write size */
	if (count != EDGEVIB_INJECT_HEADER_SZ + expected_size)
		return -EINVAL;

	/* 6. Allocate and copy frame data */
	new_frame = vmalloc(expected_size);
	if (!new_frame)
		return -ENOMEM;

	ret = copy_from_user(new_frame, buf + EDGEVIB_INJECT_HEADER_SZ,
			     expected_size);
	if (ret) {
		vfree(new_frame);
		return -EFAULT;
	}

	/* 7. Swap frame under lock, deliver to streaming consumer */
	mutex_lock(&dev->lock);

	/* Update frame metadata */
	if (dev->frame_data)
		vfree(dev->frame_data);
	dev->frame_data  = new_frame;
	dev->width       = hdr.width;
	dev->height      = hdr.height;
	dev->pixelformat = hdr.pixelformat;

	/* If a buffer is queued and streaming, deliver immediately */
	if (dev->streaming && dev->queued_buf) {
		struct vb2_buffer *vb = &dev->queued_buf->vb2_buf;
		void *vaddr = vb2_plane_vaddr(vb, 0);

		if (vaddr) {
			memcpy(vaddr, dev->frame_data, expected_size);
			vb2_set_plane_payload(vb, 0, expected_size);
			vb->timestamp = ktime_get_ns();
			vb2_buffer_done(vb, VB2_BUF_STATE_DONE);
		} else {
			vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		}
		dev->queued_buf = NULL;
	}

	mutex_unlock(&dev->lock);

	/* 8. Update global counters */
	atomic_inc(&priv->frame_count);
	atomic64_set(&priv->last_frame_time_ms,
		     (long long)jiffies_to_msecs(jiffies));

	return count;
}

static const struct file_operations edgevib_inject_fops = {
	.owner   = THIS_MODULE,
	.open    = edgevib_inject_open,
	.release = edgevib_inject_release,
	.write   = edgevib_inject_write,
};

/* ─── Custom sysfs attributes (on cdev device, D5 pattern) ─── */

static ssize_t frame_count_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	if (!g_priv)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", atomic_read(&g_priv->frame_count));
}

static ssize_t last_frame_time_ms_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	if (!g_priv)
		return -ENODEV;
	return sysfs_emit(buf, "%lld\n",
			  (long long)atomic64_read(&g_priv->last_frame_time_ms));
}

static DEVICE_ATTR_RO(frame_count);
static DEVICE_ATTR_RO(last_frame_time_ms);

static struct attribute *edgevib_video_sysfs_attrs[] = {
	&dev_attr_frame_count.attr,
	&dev_attr_last_frame_time_ms.attr,
	NULL,
};
ATTRIBUTE_GROUPS(edgevib_video_sysfs);

/* ─── Module init / exit (D5 HWMON goto-chain pattern) ─── */

static int __init edgevib_video_init(void)
{
	struct edgevib_video_priv *priv;
	int ret, i;

	/* Validate module param */
	if (num_devices < 1 || num_devices > EDGEVIB_MAX_DEVICES) {
		pr_err("edgevib_video: num_devices must be 1-%d (got %d)\n",
		       EDGEVIB_MAX_DEVICES, num_devices);
		return -EINVAL;
	}

	/* Allocate private data + device array */
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->num_devices = num_devices;

	priv->devices = kcalloc(num_devices, sizeof(struct edgevib_video_dev),
				GFP_KERNEL);
	if (!priv->devices) {
		ret = -ENOMEM;
		goto err_free_priv;
	}

	/* Initialize each device */
	for (i = 0; i < num_devices; i++) {
		struct edgevib_video_dev *dev = &priv->devices[i];

		dev->id     = i;
		dev->active = true;
		snprintf(dev->name, sizeof(dev->name),
			 "EdgeVib Virtual Cam (dev%d)", i);
		dev->width       = 640;
		dev->height      = 480;
		dev->pixelformat = V4L2_PIX_FMT_YUYV;
		mutex_init(&dev->lock);
	}

	atomic_set(&priv->frame_count, 0);
	atomic64_set(&priv->last_frame_time_ms, 0);

	/* ── Step 1: Register cdev for injection (D5 pattern) ── */
	ret = alloc_chrdev_region(&priv->cdev_num, 0, 1,
				  EDGEVIB_INJECT_DEV_NAME);
	if (ret) {
		pr_err("edgevib_video: alloc_chrdev_region failed (err=%d)\n",
		       ret);
		goto err_free_devices;
	}

	cdev_init(&priv->cdev, &edgevib_inject_fops);
	priv->cdev.owner = THIS_MODULE;
	ret = cdev_add(&priv->cdev, priv->cdev_num, 1);
	if (ret) {
		pr_err("edgevib_video: cdev_add failed (err=%d)\n", ret);
		goto err_unregister_chrdev;
	}

	priv->cdev_class = class_create(EDGEVIB_INJECT_DEV_NAME);
	if (IS_ERR(priv->cdev_class)) {
		ret = PTR_ERR(priv->cdev_class);
		pr_err("edgevib_video: class_create failed (err=%d)\n", ret);
		goto err_del_cdev;
	}

	priv->cdev_device = device_create_with_groups(
		priv->cdev_class, NULL, priv->cdev_num, priv,
		edgevib_video_sysfs_groups, EDGEVIB_INJECT_DEV_NAME);
	if (IS_ERR(priv->cdev_device)) {
		ret = PTR_ERR(priv->cdev_device);
		pr_err("edgevib_video: device_create_with_groups failed (err=%d)\n",
		       ret);
		goto err_destroy_class;
	}

	/* ── Step 2: Register V4L2 video devices (one per virtual camera) ── */
	for (i = 0; i < num_devices; i++) {
		struct edgevib_video_dev *dev = &priv->devices[i];
		struct video_device *vdev;
		struct vb2_queue *q;

		/* Allocate and configure video_device */
		vdev = video_device_alloc();
		if (!vdev) {
			ret = -ENOMEM;
			pr_err("edgevib_video: video_device_alloc failed for dev %d\n", i);
			goto err_unregister_video;
		}
		dev->vdev = vdev;

		/* Init v4l2_device parent */
		snprintf(dev->v4l2_dev.name, sizeof(dev->v4l2_dev.name),
			 "edgevib-video-%d", i);
		ret = v4l2_device_register(NULL, &dev->v4l2_dev);
		if (ret) {
			pr_err("edgevib_video: v4l2_device_register failed for dev %d (err=%d)\n",
			       i, ret);
			video_device_release(vdev);
			dev->vdev = NULL;
			goto err_unregister_video;
		}

		strscpy(vdev->name, dev->name, sizeof(vdev->name));
		vdev->v4l2_dev  = &dev->v4l2_dev;
		vdev->fops      = &edgevib_video_fops;
		vdev->ioctl_ops = &edgevib_ioctl_ops;
		vdev->release   = video_device_release;
		vdev->lock      = &dev->lock;           /* REQUIRED: video_ioctl2 serialization */
		vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
		vdev->vfl_dir   = VFL_DIR_RX;
		video_set_drvdata(vdev, dev);

		/* Init vb2_queue with VMALLOC memops */
		q = &dev->vb2_q;
		q->type            = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		q->io_modes        = VB2_MMAP | VB2_READ;
		q->drv_priv        = dev;
		q->buf_struct_size = sizeof(struct vb2_v4l2_buffer);
		q->ops             = &edgevib_vb2_ops;
		q->mem_ops         = &vb2_vmalloc_memops;
		q->min_buffers_needed = 1;
		q->timestamp_flags  = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
		q->lock             = &dev->lock;

		ret = vb2_queue_init(q);
		if (ret) {
			pr_err("edgevib_video: vb2_queue_init failed for dev %d (err=%d)\n",
			       i, ret);
			v4l2_device_unregister(&dev->v4l2_dev);
			video_device_release(vdev);
			dev->vdev = NULL;
			goto err_unregister_video;
		}

		/* Bind queue to video_device for vb2_fop_* helpers */
		vdev->queue = q;

		/* Register the video device */
		ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
		if (ret) {
			pr_err("edgevib_video: video_register_device failed for dev %d (err=%d)\n",
			       i, ret);
			vb2_queue_release(q);
			v4l2_device_unregister(&dev->v4l2_dev);
			video_device_release(vdev);
			dev->vdev = NULL;
			goto err_unregister_video;
		}

		pr_info("edgevib_video: dev %d registered as /dev/video%d (%dx%d YUYV)\n",
			i, vdev->num, dev->width, dev->height);
	}

	g_priv = priv;
	pr_info("edgevib_video: loaded, %d devices, inject=/dev/%s\n",
		num_devices, EDGEVIB_INJECT_DEV_NAME);
	return 0;

	/* Error unwind — reverse order, goto chain (D5 pattern) */
err_unregister_video:
	for (i--; i >= 0; i--) {
		struct edgevib_video_dev *dev = &priv->devices[i];

		if (dev->vdev) {
			video_unregister_device(dev->vdev);
			/* release callback handles video_device kfree */
			dev->vdev = NULL;
		}
		vb2_queue_release(&dev->vb2_q);
		v4l2_device_unregister(&dev->v4l2_dev);
		vfree(dev->frame_data);
		mutex_destroy(&dev->lock);
	}
	device_destroy(priv->cdev_class, priv->cdev_num);
err_destroy_class:
	class_destroy(priv->cdev_class);
err_del_cdev:
	cdev_del(&priv->cdev);
err_unregister_chrdev:
	unregister_chrdev_region(priv->cdev_num, 1);
err_free_devices:
	kfree(priv->devices);
err_free_priv:
	kfree(priv);
	return ret;
}

static void __exit edgevib_video_exit(void)
{
	struct edgevib_video_priv *priv = g_priv;
	int i;

	if (!priv)
		return;

	for (i = 0; i < priv->num_devices; i++) {
		struct edgevib_video_dev *dev = &priv->devices[i];

		if (dev->vdev) {
			video_unregister_device(dev->vdev);
			dev->vdev = NULL;
		}
		vb2_queue_release(&dev->vb2_q);
		v4l2_device_unregister(&dev->v4l2_dev);
		vfree(dev->frame_data);
		mutex_destroy(&dev->lock);
	}

	device_destroy(priv->cdev_class, priv->cdev_num);
	class_destroy(priv->cdev_class);
	cdev_del(&priv->cdev);
	unregister_chrdev_region(priv->cdev_num, 1);

	kfree(priv->devices);
	kfree(priv);
	g_priv = NULL;

	pr_info("edgevib_video: unloaded\n");
}

module_init(edgevib_video_init);
module_exit(edgevib_video_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("EdgeVib Team");
MODULE_DESCRIPTION("EdgeVib Virtual V4L2 Video Capture Device — multi-device YUYV frame injector");
