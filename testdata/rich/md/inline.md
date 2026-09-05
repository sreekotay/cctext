# Inline marks

This has **bold** and *italic* words in one line.
Nesting: ***both*** and **outer *inner* outer** and *outer **inner** outer*.
Code keeps stars: `a * b` and `**not bold**` stay literal.
Escaped: \*not italic\* and \*\*not bold\*\*.
Punctuation: **bold,** then *italic.* then **(parens)** and *"quoted"*.
**Starts the line** and ends the line with **bold**
*Italic at start* and ends with *italic*
Unterminated **bold at EOL
Math: 2 * 3 = 6 and 4 * 5 * 6 = 120.
Underscores: _italic_ and __bold__ and snake_case_name stays plain.
Mixed: **bold with `code` inside** and *italic with `code` inside*.
Adjacent: **a****b** and *x**y**z*.
