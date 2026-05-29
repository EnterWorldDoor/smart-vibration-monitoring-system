package db

import (
	"context"
	"encoding/json"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

type ModelVersion struct {
	ID          int64           `json:"id"`
	ModelName   string          `json:"model_name"`
	Version     string          `json:"version"`
	FilePath    string          `json:"file_path"`
	FileSize    int64           `json:"file_size"`
	SHA256      string          `json:"sha256"`
	MetricsJSON json.RawMessage `json:"metrics_json"`
	DeployedAt  *time.Time      `json:"deployed_at"`
	DeployedBy  string          `json:"deployed_by"`
	UploadedAt  time.Time       `json:"uploaded_at"`
	Platform    string          `json:"platform"`
}

type Client struct {
	pool *pgxpool.Pool
}

func NewClient(pool *pgxpool.Pool) *Client {
	return &Client{pool: pool}
}

func (c *Client) PingHealth(ctx context.Context) error {
	return c.pool.Ping(ctx)
}

func (c *Client) InsertModelVersion(ctx context.Context, mv *ModelVersion) (int64, error) {
	sql := `INSERT INTO model_versions (model_name, version, file_path, file_size, sha256, metrics_json, deployed_at, deployed_by, platform)
	        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
	        ON CONFLICT (model_name, version)
	        DO UPDATE SET file_path=$3, file_size=$4, sha256=$5, metrics_json=$6, platform=$9, uploaded_at=NOW()
	        RETURNING id`
	var id int64
	err := c.pool.QueryRow(ctx, sql,
		mv.ModelName, mv.Version, mv.FilePath, mv.FileSize, mv.SHA256,
		mv.MetricsJSON, mv.DeployedAt, mv.DeployedBy, mv.Platform,
	).Scan(&id)
	return id, err
}

func (c *Client) GetModelVersion(ctx context.Context, modelName, version string) (*ModelVersion, error) {
	sql := `SELECT id, model_name, version, file_path, file_size, sha256, metrics_json, deployed_at, deployed_by, uploaded_at, platform
	        FROM model_versions WHERE model_name = $1 AND version = $2`
	var mv ModelVersion
	err := c.pool.QueryRow(ctx, sql, modelName, version).Scan(
		&mv.ID, &mv.ModelName, &mv.Version, &mv.FilePath, &mv.FileSize,
		&mv.SHA256, &mv.MetricsJSON, &mv.DeployedAt, &mv.DeployedBy, &mv.UploadedAt,
			&mv.Platform,
	)
	if err == pgx.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	return &mv, nil
}

func (c *Client) ListModelVersions(ctx context.Context, modelName string) ([]ModelVersion, error) {
	sql := `SELECT id, model_name, version, file_path, file_size, sha256, metrics_json, deployed_at, deployed_by, uploaded_at, platform
	        FROM model_versions WHERE model_name = $1 ORDER BY uploaded_at DESC`
	rows, err := c.pool.Query(ctx, sql, modelName)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var versions []ModelVersion
	for rows.Next() {
		var mv ModelVersion
		if err := rows.Scan(
			&mv.ID, &mv.ModelName, &mv.Version, &mv.FilePath, &mv.FileSize,
			&mv.SHA256, &mv.MetricsJSON, &mv.DeployedAt, &mv.DeployedBy, &mv.UploadedAt,
			&mv.Platform,
		); err != nil {
			return nil, err
		}
		versions = append(versions, mv)
	}
	return versions, rows.Err()
}

func (c *Client) ListAllModels(ctx context.Context) ([]ModelVersion, error) {
	sql := `SELECT DISTINCT ON (model_name) id, model_name, version, file_path, file_size, sha256, metrics_json, deployed_at, deployed_by, uploaded_at, platform
	        FROM model_versions ORDER BY model_name, uploaded_at DESC`
	rows, err := c.pool.Query(ctx, sql)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var versions []ModelVersion
	for rows.Next() {
		var mv ModelVersion
		if err := rows.Scan(
			&mv.ID, &mv.ModelName, &mv.Version, &mv.FilePath, &mv.FileSize,
			&mv.SHA256, &mv.MetricsJSON, &mv.DeployedAt, &mv.DeployedBy, &mv.UploadedAt,
			&mv.Platform,
		); err != nil {
			return nil, err
		}
		versions = append(versions, mv)
	}
	return versions, rows.Err()
}

func (c *Client) MarkDeployed(ctx context.Context, modelName, version, deployedBy string) error {
	sql := `UPDATE model_versions SET deployed_at = NOW(), deployed_by = $3 WHERE model_name = $1 AND version = $2`
	_, err := c.pool.Exec(ctx, sql, modelName, version, deployedBy)
	return err
}

func (c *Client) GetDeployedVersion(ctx context.Context, modelName string) (*ModelVersion, error) {
	sql := `SELECT id, model_name, version, file_path, file_size, sha256, metrics_json, deployed_at, deployed_by, uploaded_at, platform
	        FROM model_versions WHERE model_name = $1 AND deployed_at IS NOT NULL ORDER BY deployed_at DESC LIMIT 1`
	var mv ModelVersion
	err := c.pool.QueryRow(ctx, sql, modelName).Scan(
		&mv.ID, &mv.ModelName, &mv.Version, &mv.FilePath, &mv.FileSize,
		&mv.SHA256, &mv.MetricsJSON, &mv.DeployedAt, &mv.DeployedBy, &mv.UploadedAt,
			&mv.Platform,
	)
	if err == pgx.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	return &mv, nil
}
