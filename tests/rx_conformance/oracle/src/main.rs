// Rust `regex` oracle for tests/rx_conformance: reads cases on stdin, one
// per line, `name \t pattern \t haystack` (fields with `\xNN` escapes for
// `\`, tab, newline and controls; the haystack must be UTF-8), and writes
// regex-test TOML (captures_iter, leftmost-first) that convert.py turns
// into an rx_conform fixture. A pattern Rust rejects is written with
// `compiles = false`.
//
//   cargo run --release --manifest-path tests/rx_conformance/oracle/Cargo.toml \
//       < cases.tsv > cases.toml
use std::io::{self, BufRead, Write};

fn unesc(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'\\' && i + 3 < b.len() + 0 && b[i + 1] == b'x' {
            if let Ok(v) = u8::from_str_radix(std::str::from_utf8(&b[i + 2..i + 4]).unwrap_or("zz"), 16) {
                out.push(v);
                i += 4;
                continue;
            }
        }
        out.push(b[i]);
        i += 1;
    }
    out
}

fn tstr(s: &str) -> String {
    let mut o = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            c if (c as u32) < 0x20 || c as u32 == 0x7f => o.push_str(&format!("\\u{:04X}", c as u32)),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

fn main() {
    let stdin = io::stdin();
    let mut out = io::BufWriter::new(io::stdout());
    for line in stdin.lock().lines() {
        let line = line.unwrap();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let f: Vec<&str> = line.split('\t').collect();
        if f.len() < 3 {
            continue;
        }
        let pat = String::from_utf8(unesc(f[1])).expect("pattern utf8");
        let hay = String::from_utf8(unesc(f[2])).expect("haystack utf8");
        writeln!(out, "[[test]]\nname = {}\nregex = {}\nhaystack = {}", tstr(f[0]), tstr(&pat), tstr(&hay)).unwrap();
        let re = regex::RegexBuilder::new(&pat).size_limit(1 << 26).build();
        match re {
            Err(_) => {
                writeln!(out, "matches = []\ncompiles = false\n").unwrap();
            }
            Ok(re) => {
                let mut ms = Vec::new();
                for caps in re.captures_iter(&hay).take(64) {
                    let g: Vec<String> = (0..caps.len())
                        .map(|k| match caps.get(k) {
                            Some(m) => format!("[{}, {}]", m.start(), m.end()),
                            None => "[]".to_string(),
                        })
                        .collect();
                    ms.push(format!("[{}]", g.join(", ")));
                }
                writeln!(out, "matches = [{}]\n", ms.join(", ")).unwrap();
            }
        }
    }
}
