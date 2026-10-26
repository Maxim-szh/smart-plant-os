// Проверка документации: относительные ссылки Markdown, индекс и структура ADR.
import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { dirname, join, normalize } from 'node:path';

let failed = 0;
const fail = (m) => { console.error('✗ ' + m); failed++; };
const ok = (m) => console.log('✓ ' + m);

const walk = (dir) => readdirSync(dir).flatMap((n) => {
  const p = join(dir, n);
  if (n === 'node_modules' || n === '.git') return [];
  return statSync(p).isDirectory() ? walk(p) : [p];
});
const mdFiles = [...walk('docs'), ...walk('spec'), 'README.md', 'AGENTS.md', 'CONTRIBUTING.md'].filter((f) => f.endsWith('.md'));

// 1. Относительные ссылки ведут на существующие файлы
let links = 0;
for (const f of mdFiles) {
  const text = readFileSync(f, 'utf8').replace(/```[\s\S]*?```/g, '');
  for (const m of text.matchAll(/\]\(([^)\s]+)\)/g)) {
    const target = m[1];
    if (/^(https?:|mailto:|#)/.test(target)) continue;
    const path = normalize(join(dirname(f), decodeURIComponent(target.split('#')[0])));
    links++;
    if (!existsSync(path)) fail(`${f}: ссылка на несуществующий файл ${target}`);
  }
}
if (!failed) ok(`ссылки Markdown: ${links} относительных ссылок в ${mdFiles.length} файлах`);

// 2. ADR: структура и индекс
const adrDir = 'docs/adr';
const adrs = readdirSync(adrDir).filter((n) => /^\d{4}-.+\.md$/.test(n)).sort();
const index = readFileSync(join(adrDir, 'README.md'), 'utf8');
const statuses = ['Принято', 'Предложено', 'Заменено', 'Отклонено'];
const before = failed;
adrs.forEach((n, i) => {
  const t = readFileSync(join(adrDir, n), 'utf8');
  const num = n.slice(0, 4);
  if (Number(num) !== i + 1) fail(`${n}: номера ADR должны идти подряд без пропусков`);
  const st = t.match(/^Статус: (.+)$/m);
  if (!st || !statuses.includes(st[1].trim())) fail(`${n}: нет корректной строки «Статус:»`);
  if (!/^Дата: \d{4}-\d{2}-\d{2}$/m.test(t)) fail(`${n}: нет строки «Дата:»`);
  for (const h of ['## Контекст', '## Решение', '## Последствия', '## Что не проверено']) if (!t.includes(h)) fail(`${n}: нет раздела «${h.slice(3)}»`);
  const row = index.split('\n').find((l) => l.includes(`(${n})`));
  if (!row) fail(`${n}: нет в индексе docs/adr/README.md`);
  else if (st && !row.includes(st[1].trim())) fail(`${n}: статус в индексе расходится с файлом`);
});
if (failed === before) ok(`ADR: ${adrs.length} записей, структура и индекс согласованы`);

process.exit(failed ? 1 : 0);
