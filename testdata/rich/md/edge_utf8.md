# UTF-8 edges

ZWJ emoji: **👨‍👩‍👧 family** stays one cluster inside bold.
Flags: *🇯🇵 Japan* and **🇩🇪🇫🇷** two regional-indicator pairs.
Combining: **é** (e + U+0301) and *ȫ* stacked marks.
Star touches a combining mark: **café** and *näive*.
Mark right after the hint: **́abc** combines with the star in source mode.
CJK bold: **日本語のテキスト** and italic *中文字符* in prose.
Mixed width: **ab日本cd** and `code日本` in one line.

| 名前 | Kind | 幅 |
|------|:----:|---:|
| **太字** | wide | 4 |
| *斜体* | wide | 4 |
| ascii | narrow | 5 |
| 🇯🇵 | flag | 2 |
| é | combining | 1 |
| 👨‍👩‍👧 | zwj | 2 |

Zero-width joiner alone: a‍b is one line.
