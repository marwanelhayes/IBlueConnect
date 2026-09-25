/// One callable number of a phone contact.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Entry {
    pub name: String,
    pub label: String,
    pub number: String,
}

/// Parses a PBAP vCard 2.1/3.0 phonebook into name/label/number entries.
pub fn parse(text: &str) -> Vec<Entry> {
    let mut entries = Vec::new();
    let mut name = String::new();
    let mut structured = String::new();
    let mut numbers: Vec<(String, String)> = Vec::new();
    for line in unfold(text) {
        let Some((head, value)) = line.split_once(':') else { continue };
        let mut params = head.split(';');
        let property = params.next().unwrap_or("");
        let property = property.rsplit('.').next().unwrap_or(property).to_ascii_uppercase();
        let params: Vec<String> = params.map(str::to_ascii_uppercase).collect();
        match property.as_str() {
            "BEGIN" => { name.clear(); structured.clear(); numbers.clear(); }
            "FN" => name = decode(value, &params).trim().to_owned(),
            "N" => structured = structured_name(&decode(value, &params)),
            "TEL" => {
                let number = tel_number(value);
                if !number.is_empty() { numbers.push((label(&params).into(), number)); }
            }
            "END" => {
                for (label, number) in numbers.drain(..) {
                    let name = [&name, &structured, &number].into_iter().find(|name| !name.is_empty()).unwrap().clone();
                    entries.push(Entry { name, label, number });
                }
            }
            _ => {}
        }
    }
    entries
}

/// "Family;Given;…" → "Given Family".
pub(crate) fn structured_name(decoded: &str) -> String {
    let parts: Vec<&str> = decoded.split(';').map(str::trim).collect();
    let given = parts.get(1).copied().unwrap_or("");
    let family = parts.first().copied().unwrap_or("");
    [given, family].iter().filter(|part| !part.is_empty()).copied().collect::<Vec<_>>().join(" ")
}

/// Keeps only dialable characters: digits, `+`, `*`, `#`.
pub(crate) fn tel_number(value: &str) -> String {
    value.chars().filter(|c| c.is_ascii_digit() || matches!(c, '+' | '*' | '#')).collect()
}

pub(crate) fn unfold(text: &str) -> Vec<String> {
    let mut lines: Vec<String> = Vec::new();
    for line in text.split('\n') {
        let line = line.strip_suffix('\r').unwrap_or(line);
        match (line.strip_prefix([' ', '\t']), lines.last_mut()) {
            (Some(rest), Some(last)) => last.push_str(rest),
            // Quoted-printable soft line break: a trailing '=' continues on the next line.
            (_, Some(last)) if last.ends_with('=') && last.to_ascii_uppercase().contains("QUOTED-PRINTABLE") => {
                last.pop();
                last.push_str(line);
            }
            _ => lines.push(line.to_owned()),
        }
    }
    lines
}

fn label(params: &[String]) -> &'static str {
    let types = params.iter().flat_map(|param| {
        param.strip_prefix("TYPE=").unwrap_or(param).split(',').map(str::to_owned).collect::<Vec<_>>()
    });
    let mut label = "Phone";
    for kind in types {
        label = match kind.as_str() {
            "IPHONE" => return "iPhone",
            "CELL" => "Mobile",
            "HOME" if label == "Phone" => "Home",
            "WORK" if label == "Phone" => "Work",
            "MAIN" if label == "Phone" => "Main",
            _ => label,
        };
    }
    label
}

pub(crate) fn decode(value: &str, params: &[String]) -> String {
    if !params.iter().any(|param| param.ends_with("QUOTED-PRINTABLE")) {
        return value.to_owned();
    }
    let bytes = value.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        let hex = bytes.get(i + 1..i + 3).filter(|pair| pair.iter().all(u8::is_ascii_hexdigit))
            .and_then(|pair| std::str::from_utf8(pair).ok())
            .and_then(|pair| u8::from_str_radix(pair, 16).ok());
        match (bytes[i], hex) {
            (b'=', Some(byte)) => { out.push(byte); i += 3; }
            (byte, _) => { out.push(byte); i += 1; }
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(name: &str, label: &str, number: &str) -> Entry {
        Entry { name: name.into(), label: label.into(), number: number.into() }
    }

    #[test]
    fn card_with_two_numbers_yields_two_labelled_entries() {
        let text = "BEGIN:VCARD\r\nVERSION:3.0\r\nFN:Alice Smith\r\nTEL;TYPE=CELL;TYPE=VOICE:+20 100 123 4567\r\nTEL;TYPE=HOME:02-555\r\nEND:VCARD\r\n";
        assert_eq!(parse(text), vec![entry("Alice Smith", "Mobile", "+201001234567"), entry("Alice Smith", "Home", "02555")]);
    }

    #[test]
    fn handles_v21_params_grouped_tel_and_folded_lines() {
        let text = "BEGIN:VCARD\nVERSION:2.1\nFN:Bob\n  Jones\nTEL;CELL:111\nitem1.TEL;type=IPHONE;type=CELL:222\nTEL;WORK:3\n 33\nEND:VCARD\n";
        assert_eq!(parse(text), vec![entry("Bob Jones", "Mobile", "111"), entry("Bob Jones", "iPhone", "222"), entry("Bob Jones", "Work", "333")]);
    }

    #[test]
    fn falls_back_to_n_decodes_quoted_printable_and_skips_numberless_cards() {
        let text = concat!(
            "BEGIN:VCARD\nVERSION:2.1\nN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:=D8=B9=D9=84=D9=8A;Ahmed;;;\nTEL:1\nEND:VCARD\n",
            "BEGIN:VCARD\nVERSION:3.0\nFN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:Caf=C3=A9 =\nBar\nTEL:2\nEND:VCARD\n",
            "BEGIN:VCARD\nVERSION:3.0\nFN:No Number\nEND:VCARD\n",
            "BEGIN:VCARD\nVERSION:3.0\nTEL;TYPE=MAIN:+33\nEND:VCARD\n",
        );
        assert_eq!(parse(text), vec![
            entry("Ahmed \u{639}\u{644}\u{64a}", "Phone", "1"),
            entry("Caf\u{e9} Bar", "Phone", "2"),
            entry("+33", "Main", "+33"),
        ]);
    }
}
