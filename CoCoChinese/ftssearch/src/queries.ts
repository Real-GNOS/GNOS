import type { ColumnMapping, SearchOptions } from './types.js'
import { quoteIdent, buildTsvExpression, buildNewTsvExpression, buildLikeConditions } from './utils.js'
import { WEIGHT_VALUES } from './types.js'

export function sqlCreateTriggerFunction(table: string, columns: ColumnMapping[], language: string): string {
  const funcName = `${quoteIdent(table)}_tsv_update`
  const tsvExpr = buildNewTsvExpression(columns, language)
  return `
    CREATE OR REPLACE FUNCTION ${funcName}() RETURNS trigger AS $$
    BEGIN
      NEW.tsv := ${tsvExpr};
      RETURN NEW;
    END;
    $$ LANGUAGE plpgsql
  `
}

export function sqlCreateTrigger(table: string): string {
  const funcName = `${quoteIdent(table)}_tsv_update`
  return `
    DROP TRIGGER IF EXISTS trg_${quoteIdent(table)}_tsv ON ${quoteIdent(table)}
  `
}

export function sqlCreateTrigger2(table: string): string {
  const funcName = `${quoteIdent(table)}_tsv_update`
  return `
    CREATE TRIGGER trg_${quoteIdent(table)}_tsv
    BEFORE INSERT OR UPDATE ON ${quoteIdent(table)}
    FOR EACH ROW EXECUTE FUNCTION ${funcName}()
  `
}

export function sqlAddTsvColumn(table: string): string {
  return `ALTER TABLE ${quoteIdent(table)} ADD COLUMN IF NOT EXISTS tsv tsvector`
}

export function sqlCreateIndex(table: string): string {
  return `CREATE INDEX IF NOT EXISTS idx_${quoteIdent(table)}_tsv ON ${quoteIdent(table)} USING GIN(tsv)`
}

export function sqlReindex(table: string, columns: ColumnMapping[], language: string): string {
  const tsvExpr = buildTsvExpression(columns, language)
  return `UPDATE ${quoteIdent(table)} SET tsv = ${tsvExpr} WHERE true`
}

export function sqlDropTrigger(table: string): string {
  return `DROP TRIGGER IF EXISTS trg_${quoteIdent(table)}_tsv ON ${quoteIdent(table)}`
}

export function sqlDropFunction(table: string): string {
  return `DROP FUNCTION IF EXISTS ${quoteIdent(table)}_tsv_update()`
}

export function sqlDropIndex(table: string): string {
  return `DROP INDEX IF EXISTS idx_${quoteIdent(table)}_tsv`
}

export function sqlDropTsvColumn(table: string): string {
  return `ALTER TABLE ${quoteIdent(table)} DROP COLUMN IF EXISTS tsv`
}

export function sqlSearch(
  table: string,
  columns: ColumnMapping[],
  options: Required<SearchOptions>,
  language: string,
): { query: string; countQuery: string } {
  const weights = options.weights
  const weightArray = `{${weights[0]},${weights[1]},${weights[2]},${weights[3]}}`
  const norm = options.normalize
  const tsv = '"tsv"'
  const likeClauses = buildLikeConditions(columns, 2)

  const rankExpr = `
    CASE WHEN v.${tsv} @@ q.query
      THEN ts_rank('${weightArray}'::float4[], v.${tsv}, q.query, ${norm})
      ELSE 0
    END +
    CASE WHEN v.${quoteIdent(columns[0].column)}::text ILIKE $2 THEN 3 ELSE 0 END`

  const whereClause = `
    (q.query IS NOT NULL AND v.${tsv} @@ q.query)
    OR ${likeClauses.join('\n    OR ')}`

  const query = `
    WITH q AS (SELECT plainto_tsquery('${language}', $1) AS query)
    SELECT v.*, (${rankExpr}) AS rank
    FROM ${quoteIdent(table)} v, q
    WHERE ${whereClause}
    ORDER BY rank DESC, v.created_at DESC NULLS LAST
    OFFSET $3 LIMIT $4
  `

  const countQuery = `
    WITH q AS (SELECT plainto_tsquery('${language}', $1) AS query)
    SELECT count(*) FROM ${quoteIdent(table)} v, q
    WHERE ${whereClause}
  `

  return { query, countQuery }
}
