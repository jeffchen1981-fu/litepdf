# coding: utf-8
"""Generate installer/LICENSE-DISPLAY.rtf from the canonical zh-TW text.
Non-ASCII chars become \\uNNNN? RTF escapes so the .rtf is pure ASCII and
renders correctly in Inno Setup's InfoBeforeFile page. Re-run after edits."""
import os

CONTENT = """\
─────────────────────────────────────────────
LitePDF 授權條款
─────────────────────────────────────────────

LitePDF 依 GNU Affero General Public License v3.0 (AGPL-3.0) 發佈。
本條款保障您以下權利:
  • 可自由使用本程式 (個人或商業用途均可)
  • 可取得完整原始碼: https://github.com/jeffchen1981-fu/litepdf
  • 可修改並再散佈本程式,但需沿用相同授權

若您將本程式或其衍生作品透過網路提供服務,
AGPL 要求您同樣需將您的原始碼公開 (AGPL §13)。

完整英文條款: https://www.gnu.org/licenses/agpl-3.0.html

─────────────────────────────────────────────
本程式包含之第三方元件
─────────────────────────────────────────────

• MuPDF 1.27.2 — (c) Artifex Software, Inc. — AGPL-3.0
• FreeType 2.13.3 — The FreeType Project — FTL
• libjpeg 9f — Independent JPEG Group — IJG License
• OpenJPEG — UCLouvain / OpenJPEG contributors — BSD-2-Clause
• Little CMS (lcms2) — (c) Marti Maria — MIT
• MuJS — (c) Artifex Software, Inc. — ISC
• jbig2dec — (c) Artifex Software, Inc. — AGPL-3.0
• Gumbo (gumbo-parser) — (c) Google, Inc. — Apache-2.0
• zlib 1.3.1 — (c) Jean-loup Gailly & Mark Adler — zlib License

Portions of this software are copyright (c) 2006-2024 The FreeType Project (www.freetype.org). All rights reserved.
This software is based in part on the work of the Independent JPEG Group.

─────────────────────────────────────────────
免責聲明
─────────────────────────────────────────────

本程式按「原樣」提供,不含任何明示或默示保證。
作者與貢獻者對任何使用後果不負責任。
"""

def rtf_escape(s):
    out = []
    for ch in s:
        o = ord(ch)
        if ch == '\\':
            out.append(r'\\')
        elif ch == '{':
            out.append(r'\{')
        elif ch == '}':
            out.append(r'\}')
        elif ch == '\n':
            out.append('\\par\n')
        elif o < 128:
            out.append(ch)
        else:
            # RTF \uN takes a SIGNED 16-bit value; codepoints > 0x7FFF must be
            # written as their negative equivalent (o - 65536) to be spec-valid.
            signed = o if o < 32768 else o - 65536
            out.append('\\u%d?' % signed)
    return ''.join(out)

HEADER = (r'{\rtf1\ansi\ansicpg950\deff0'
          r'{\fonttbl{\f0\fnil\fcharset136 Microsoft JhengHei;}}'
          '\n' r'\viewkind4\uc1\f0\fs20' '\n')

def main():
    rtf = HEADER + rtf_escape(CONTENT) + '\n}\n'
    out_path = os.path.join(os.path.dirname(__file__), 'LICENSE-DISPLAY.rtf')
    with open(out_path, 'w', encoding='ascii', newline='') as f:
        f.write(rtf)
    print('wrote', out_path, len(rtf), 'bytes')

if __name__ == '__main__':
    main()
