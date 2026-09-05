// cctext embed fixture: template literals, tagged templates, regex vs division.
const name = "world";
const plain = `hello ${name} and ${1 + 2} done`;

const q = sql`SELECT id, name FROM users WHERE id = ${userId} AND active = 1`;
const view = html`<div class="card"><span>${name}</span></div>`;

const re = /ab+c/gi;
const ratio = a / b / c;
const notRe = total / count; // division, not a regex literal

// TSX-style JSX snippet (this file is plain .js; a TSX grammar would embed it):
// <Foo bar="x">{y}</Foo>

const ticks = "string with `backticks` inside";
const alsoTicks = 'and \'single\' with ` too';

const doc = `
# Title

Some prose, then a fence:

\`\`\`python
def f():
    """docstring inside a template inside a fence"""
    return 1
\`\`\`

end of template`;

export { plain, q, view, re, ratio, notRe, ticks, alsoTicks, doc };
