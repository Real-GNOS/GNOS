import type { ColumnMapping, Weight } from './types.js'

const WEIGHT_LABELS: Record<Weight, string> = {
  A: 'A',
  B: 'B',
  C: 'C',
  D: 'D',
}

export function quoteIdent(id: string): string {
  return '"' + id.replace(/"/g, '""') + '"'
}

export function buildTsvExpression(columns: ColumnMapping[], language: string): string {
  const parts = columns.map(({ column, weight, isArray }) => {
    const col = quoteIdent(column)
    const expr = isArray
      ? `coalesce(array_to_string(${col}, ' '), '')`
      : `coalesce(${col}::text, '')`
    return `setweight(to_tsvector('${language}', ${expr}), '${WEIGHT_LABELS[weight]}')`
  })
  return parts.join(' || ')
}

export function buildNewTsvExpression(columns: ColumnMapping[], language: string): string {
  const parts = columns.map(({ column, weight, isArray }) => {
    const col = quoteIdent(column)
    const expr = isArray
      ? `coalesce(array_to_string(NEW.${col}, ' '), '')`
      : `coalesce(NEW.${col}::text, '')`
    return `setweight(to_tsvector('${language}', ${expr}), '${WEIGHT_LABELS[weight]}')`
  })
  return parts.join(' || ')
}

export function buildLikeConditions(columns: ColumnMapping[], paramIndex: number): string[] {
  return columns.map(({ column, isArray }) => {
    const col = quoteIdent(column)
    if (isArray) {
      return `EXISTS (SELECT 1 FROM unnest(${col}) t WHERE t ILIKE $${paramIndex})`
    }
    return `${col}::text ILIKE $${paramIndex}`
  })
}
