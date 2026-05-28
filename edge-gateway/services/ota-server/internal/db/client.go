package db

import (
	"context"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type Client struct {
	pool *pgxpool.Pool
}

func NewClient(pool *pgxpool.Pool) *Client {
	return &Client{pool: pool}
}

func (c *Client) Ping(ctx context.Context) error {
	return c.pool.Ping(ctx)
}

func (c *Client) PingHealth(ctx context.Context) error {
	return c.pool.Ping(ctx)
}

type FirmwareVersion struct {
	ID             int64     `json:"id"`
	Platform       string    `json:"platform"`
	Version        string    `json:"version"`
	BuildDate      string    `json:"build_date"`
	FileName       string    `json:"file_name"`
	FileSize       int64     `json:"file_size"`
	SHA256         string    `json:"sha256"`
	MinHardwareRev string    `json:"min_hardware_rev"`
	ReleaseNotes   string    `json:"release_notes"`
	UploadedAt     time.Time `json:"uploaded_at"`
	FilePath       string    `json:"file_path"`
}

type UpgradeHistory struct {
	ID          int64      `json:"id"`
	Platform    string     `json:"platform"`
	DeviceID    string     `json:"device_id"`
	SiteID      string     `json:"site_id"`
	FromVersion string     `json:"from_version"`
	ToVersion   string     `json:"to_version"`
	Status      string     `json:"status"`
	Progress    int        `json:"progress"`
	ErrorMsg    *string    `json:"error_msg,omitempty"`
	StartedAt   time.Time  `json:"started_at"`
	CompletedAt *time.Time `json:"completed_at,omitempty"`
	DurationMs  *int64     `json:"duration_ms,omitempty"`
}

func (c *Client) InsertFirmwareVersion(ctx context.Context, fw *FirmwareVersion) (int64, error) {
	var id int64
	err := c.pool.QueryRow(ctx,
		`INSERT INTO firmware_versions (platform, version, build_date, file_name, file_size, sha256, min_hardware_rev, release_notes, file_path)
		 VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
		 ON CONFLICT (platform, version) DO UPDATE SET
		     build_date = EXCLUDED.build_date,
		     file_name = EXCLUDED.file_name,
		     file_size = EXCLUDED.file_size,
		     sha256 = EXCLUDED.sha256,
		     min_hardware_rev = EXCLUDED.min_hardware_rev,
		     release_notes = EXCLUDED.release_notes,
		     file_path = EXCLUDED.file_path,
		     uploaded_at = NOW()
		 RETURNING id`,
		fw.Platform, fw.Version, fw.BuildDate, fw.FileName,
		fw.FileSize, fw.SHA256, fw.MinHardwareRev, fw.ReleaseNotes, fw.FilePath,
	).Scan(&id)
	if err != nil {
		return 0, fmt.Errorf("insert firmware version: %w", err)
	}
	return id, nil
}

func (c *Client) GetLatestVersion(ctx context.Context, platform string) (*FirmwareVersion, error) {
	var fw FirmwareVersion
	err := c.pool.QueryRow(ctx,
		`SELECT id, platform, version, build_date, file_name, file_size, sha256, min_hardware_rev, release_notes, uploaded_at, file_path
		 FROM firmware_versions
		 WHERE platform = $1
		 ORDER BY uploaded_at DESC
		 LIMIT 1`,
		platform,
	).Scan(&fw.ID, &fw.Platform, &fw.Version, &fw.BuildDate, &fw.FileName,
		&fw.FileSize, &fw.SHA256, &fw.MinHardwareRev, &fw.ReleaseNotes, &fw.UploadedAt, &fw.FilePath)
	if err != nil {
		if err == pgx.ErrNoRows {
			return nil, nil
		}
		return nil, fmt.Errorf("get latest version: %w", err)
	}
	return &fw, nil
}

func (c *Client) GetLatestAllPlatforms(ctx context.Context) (map[string]*FirmwareVersion, error) {
	rows, err := c.pool.Query(ctx,
		`SELECT DISTINCT ON (platform) id, platform, version, build_date, file_name, file_size, sha256, min_hardware_rev, release_notes, uploaded_at, file_path
		 FROM firmware_versions
		 ORDER BY platform, uploaded_at DESC`)
	if err != nil {
		return nil, fmt.Errorf("get latest all platforms: %w", err)
	}
	defer rows.Close()

	result := make(map[string]*FirmwareVersion)
	for rows.Next() {
		var fw FirmwareVersion
		if err := rows.Scan(&fw.ID, &fw.Platform, &fw.Version, &fw.BuildDate, &fw.FileName,
			&fw.FileSize, &fw.SHA256, &fw.MinHardwareRev, &fw.ReleaseNotes, &fw.UploadedAt, &fw.FilePath); err != nil {
			return nil, fmt.Errorf("scan firmware version: %w", err)
		}
		result[fw.Platform] = &fw
	}
	return result, rows.Err()
}

func (c *Client) ListVersions(ctx context.Context, platform string) ([]FirmwareVersion, error) {
	query := `SELECT id, platform, version, build_date, file_name, file_size, sha256, min_hardware_rev, release_notes, uploaded_at, file_path
		 FROM firmware_versions`
	args := []interface{}{}
	if platform != "" {
		query += ` WHERE platform = $1`
		args = append(args, platform)
	}
	query += ` ORDER BY uploaded_at DESC`

	rows, err := c.pool.Query(ctx, query, args...)
	if err != nil {
		return nil, fmt.Errorf("list versions: %w", err)
	}
	defer rows.Close()

	var versions []FirmwareVersion
	for rows.Next() {
		var fw FirmwareVersion
		if err := rows.Scan(&fw.ID, &fw.Platform, &fw.Version, &fw.BuildDate, &fw.FileName,
			&fw.FileSize, &fw.SHA256, &fw.MinHardwareRev, &fw.ReleaseNotes, &fw.UploadedAt, &fw.FilePath); err != nil {
			return nil, fmt.Errorf("scan firmware version: %w", err)
		}
		versions = append(versions, fw)
	}
	return versions, rows.Err()
}

func (c *Client) InsertUpgradeHistory(ctx context.Context, h *UpgradeHistory) (int64, error) {
	var id int64
	err := c.pool.QueryRow(ctx,
		`INSERT INTO upgrade_history (platform, device_id, site_id, from_version, to_version, status, progress, error_msg)
		 VALUES ($1, $2, $3, $4, $5, $6, $7, $8)
		 RETURNING id`,
		h.Platform, h.DeviceID, h.SiteID, h.FromVersion, h.ToVersion, h.Status, h.Progress, h.ErrorMsg,
	).Scan(&id)
	if err != nil {
		return 0, fmt.Errorf("insert upgrade history: %w", err)
	}
	return id, nil
}

func (c *Client) UpdateUpgradeStatus(ctx context.Context, id int64, status string, progress int, errorMsg *string) error {
	_, err := c.pool.Exec(ctx,
		`UPDATE upgrade_history
		 SET status = $2, progress = $3, error_msg = $4,
		     completed_at = CASE WHEN $2 IN ('success', 'failed') THEN NOW() ELSE completed_at END,
		     duration_ms = CASE WHEN $2 IN ('success', 'failed')
		         THEN EXTRACT(EPOCH FROM (NOW() - started_at)) * 1000
		         ELSE duration_ms END
		 WHERE id = $1`,
		id, status, progress, errorMsg)
	if err != nil {
		return fmt.Errorf("update upgrade status: %w", err)
	}
	return nil
}

func (c *Client) FindPendingUpgrade(ctx context.Context, platform, deviceID, siteID string) (*UpgradeHistory, error) {
	var h UpgradeHistory
	err := c.pool.QueryRow(ctx,
		`SELECT id, platform, device_id, site_id, from_version, to_version, status, progress, error_msg, started_at, completed_at, duration_ms
		 FROM upgrade_history
		 WHERE platform = $1 AND device_id = $2 AND site_id = $3
		   AND status NOT IN ('success', 'failed')
		 ORDER BY started_at DESC
		 LIMIT 1`,
		platform, deviceID, siteID,
	).Scan(&h.ID, &h.Platform, &h.DeviceID, &h.SiteID, &h.FromVersion, &h.ToVersion,
		&h.Status, &h.Progress, &h.ErrorMsg, &h.StartedAt, &h.CompletedAt, &h.DurationMs)
	if err != nil {
		if err == pgx.ErrNoRows {
			return nil, nil
		}
		return nil, fmt.Errorf("find pending upgrade: %w", err)
	}
	return &h, nil
}

func (c *Client) ListUpgradeHistory(ctx context.Context, platform, deviceID string, limit, offset int) ([]UpgradeHistory, error) {
	query := `SELECT id, platform, device_id, site_id, from_version, to_version, status, progress, error_msg, started_at, completed_at, duration_ms
		 FROM upgrade_history
		 WHERE ($1 = '' OR platform = $1) AND ($2 = '' OR device_id = $2)
		 ORDER BY started_at DESC
		 LIMIT $3 OFFSET $4`

	rows, err := c.pool.Query(ctx, query, platform, deviceID, limit, offset)
	if err != nil {
		return nil, fmt.Errorf("list upgrade history: %w", err)
	}
	defer rows.Close()

	var history []UpgradeHistory
	for rows.Next() {
		var h UpgradeHistory
		if err := rows.Scan(&h.ID, &h.Platform, &h.DeviceID, &h.SiteID, &h.FromVersion, &h.ToVersion,
			&h.Status, &h.Progress, &h.ErrorMsg, &h.StartedAt, &h.CompletedAt, &h.DurationMs); err != nil {
			return nil, fmt.Errorf("scan upgrade history: %w", err)
		}
		history = append(history, h)
	}
	return history, rows.Err()
}
