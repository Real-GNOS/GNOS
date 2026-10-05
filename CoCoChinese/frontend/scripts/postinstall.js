#!/usr/bin/env node
const { existsSync, readFileSync, writeFileSync } = require('fs')
const { resolve } = require('path')
const { execSync } = require('child_process')

const envPath = resolve(__dirname, '..', '.env')

if (!existsSync(envPath)) {
  console.log('\n  ╔═══════════════════════════════════════════╗')
  console.log('  ║     ❖ Cocokalo Setup Required ❖         ║')
  console.log('  ╚═══════════════════════════════════════════╝')
  console.log('\n  ▶ Run the setup wizard at: http://localhost:3000/setup')
  console.log('  ▶ Or create .env manually\n')
} else {
  console.log('  ✓ Cocokalo .env found')

  try {
    execSync('npx prisma generate 2>/dev/null || true', { stdio: 'ignore', cwd: resolve(__dirname, '..') })
    console.log('  ✓ Prisma client generated')
  } catch {
    console.log('  ⚠ Prisma generation skipped')
  }
}
