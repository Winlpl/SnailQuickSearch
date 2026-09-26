/* test fixtures: 造 read_file 路径的全部测试样本 (node, 无外部依赖) */
'use strict';
const fs = require('fs'), path = require('path'), zlib = require('zlib');

const dir = path.join(process.env.TEMP, 'aft');
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });

/* ---- 最小 ZIP writer (method 0=stored 8=deflateRaw; 各条目可不同方法/级别) ---- */
function zipBuild(entries) {
  const chunks = [], cds = [];
  let off = 0;
  for (const e of entries) {
    const nameBuf = Buffer.from(e.name, 'utf8');
    const method = e.method === 0 ? 0 : (e.method || 8);
    const level = e.level == null ? 9 : e.level;
    const data = method === 8 ? zlib.deflateRawSync(e.data, { level }) : e.data;
    const crc = zlib.crc32(e.data) >>> 0;
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0); lh.writeUInt16LE(20, 4); lh.writeUInt16LE(0, 6);
    lh.writeUInt16LE(method, 8); lh.writeUInt16LE(0, 10); lh.writeUInt16LE(0, 12);
    lh.writeUInt32LE(crc, 14); lh.writeUInt32LE(data.length, 18); lh.writeUInt32LE(e.data.length, 22);
    lh.writeUInt16LE(nameBuf.length, 26); lh.writeUInt16LE(0, 28);
    chunks.push(lh, nameBuf, data);
    const cd = Buffer.alloc(46);
    cd.writeUInt32LE(0x02014b50, 0); cd.writeUInt16LE(20, 4); cd.writeUInt16LE(20, 6);
    cd.writeUInt16LE(0, 8); cd.writeUInt16LE(method, 10); cd.writeUInt16LE(0, 12); cd.writeUInt16LE(0, 14);
    cd.writeUInt32LE(crc, 16); cd.writeUInt32LE(data.length, 20); cd.writeUInt32LE(e.data.length, 24);
    cd.writeUInt16LE(nameBuf.length, 28); cd.writeUInt16LE(0, 30); cd.writeUInt16LE(0, 32);
    cd.writeUInt16LE(0, 34); cd.writeUInt16LE(0, 36); cd.writeUInt32LE(0, 38); cd.writeUInt32LE(off, 42);
    cds.push(cd, nameBuf);
    off += lh.length + nameBuf.length + data.length;
  }
  const cdStart = off;
  let cdLen = 0;
  for (const c of cds) cdLen += c.length;
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0); eocd.writeUInt16LE(entries.length, 10);
  eocd.writeUInt32LE(cdLen, 12); eocd.writeUInt32LE(cdStart, 16);
  return Buffer.concat([...chunks, ...cds, eocd]);
}
const W = (name, data) => fs.writeFileSync(path.join(dir, name), data);

/* ---- 纯文本各编码 ---- */
W('gbk.txt', Buffer.from([0xD6,0xD0,0xCE,0xC4,0xB2,0xE2,0xCA,0xD4,0xD0,0xD0,0xD2,0xBB,0x0D,0x0A,0xB5,0xDA,0xB6,0xFE,0xD0,0xD0,0x0D,0x0A]));
W('u16.txt', Buffer.concat([Buffer.from([0xFF,0xFE]), Buffer.from('宽字符 hello\r\n第二行', 'utf16le')]));
W('u8bom.txt', Buffer.concat([Buffer.from([0xEF,0xBB,0xBF]), Buffer.from('BOM中文OK', 'utf8')]));
W('plain.txt', Buffer.from('纯UTF8中文 plain-mix', 'utf8'));
W('big.txt', Buffer.concat([Buffer.alloc(30000, 0x41), Buffer.from('HEADTAIL', 'utf8'), Buffer.alloc(100000, 0x42), Buffer.from('FINALTAIL', 'utf8')]));
W('binary.bin', Buffer.from([0x61, 0x62, 0x00, 0x63]));
W('fake.pdf', Buffer.from('%PDF-1.4 fake', 'utf8'));

/* ---- 假 Office 包 ---- */
const docXml = `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body>
<w:p><w:r><w:t>第一段</w:t></w:r><w:r><w:tab/><w:t>A&amp;B</w:t></w:r></w:p>
<w:p><w:r><w:t>行一</w:t><w:br/><w:t>行二</w:t></w:r></w:p>
</w:body></w:document>`;
W('test.docx', zipBuild([
  { name: '[Content_Types].xml', data: Buffer.from('<Types/>', 'utf8') },
  { name: 'word/document.xml', data: Buffer.from(docXml, 'utf8'), level: 9 },
]));
/* stored.docx: document.xml 用 method 0 (真存储条目) + 另一压缩条目用 level 0 (deflate 内 stored 块) */
W('stored.docx', zipBuild([
  { name: 'word/document.xml', data: Buffer.from('<w:document><w:body><w:p><w:r><w:t>存储条目测试OK</w:t></w:r></w:p></w:body></w:document>', 'utf8'), method: 0 },
  { name: '[Content_Types].xml', data: Buffer.from('<Types>' + 'x'.repeat(500) + '</Types>', 'utf8'), level: 0 },
]));
const sstXml = `<?xml version="1.0"?><sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><si><t>苹果</t></si><si><r><t>香</t></r><r><t>蕉</t></r></si></sst>`;
const sheetXml = `<?xml version="1.0"?><worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>
<row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1"><v>123.5</v></c></row>
<row r="2"><c r="A2" t="s"><v>1</v></c><c r="B2" t="str"><v>formula</v></c></row>
</sheetData></worksheet>`;
W('test.xlsx', zipBuild([
  { name: 'xl/sharedStrings.xml', data: Buffer.from(sstXml, 'utf8') },
  { name: 'xl/worksheets/sheet1.xml', data: Buffer.from(sheetXml, 'utf8') },
]));
const slide1 = `<?xml version="1.0"?><p:sld xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><p:cSld><p:spTree><a:t>标题一</a:t><a:t>副标</a:t></p:spTree></p:cSld></p:sld>`;
const slide2 = `<?xml version="1.0"?><p:sld><p:cSld><p:spTree><a:t>第二页内容</a:t></p:spTree></p:cSld></p:sld>`;
W('test.pptx', zipBuild([
  { name: 'ppt/slides/slide1.xml', data: Buffer.from(slide1, 'utf8') },
  { name: 'ppt/slides/slide2.xml', data: Buffer.from(slide2, 'utf8') },
  { name: 'ppt/slides/slide10.xml', data: Buffer.from('<a:t>第十页</a:t>', 'utf8') },
]));
console.log('fixtures ready at', dir);
