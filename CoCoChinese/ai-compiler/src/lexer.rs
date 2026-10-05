#[derive(Debug, Clone, PartialEq)]
pub enum TokenKind {
    Header,
    Ganosly,
    Lomiior,
    CI,
    Dm,
    Baiun,
    ShoutToTheVoid,
    ReturnToNature,
    Revolve,
    DPrefix,
    Number(String),
    StringLit(String),
    Ident(String),
    Eq, EqEq, Gte, Lte, Gt, Lt, Neq,
    Plus, Minus, Star, Slash, Percent,
    OpenSq, CloseSq, FullOpenSq,
    OpenBrace, CloseBrace,
    OpenParen, CloseParen,
    FullOpenCorner, FullCloseCorner,
    Tilde, MapleLeaf,
    Semi, FullSemi, Comma,
    Comment(String),
}

impl std::fmt::Display for TokenKind {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            TokenKind::Header => write!(f, "Header"),
            TokenKind::Ganosly => write!(f, "Ganosly"),
            TokenKind::Lomiior => write!(f, "Lomiior"),
            TokenKind::CI => write!(f, "CI"),
            TokenKind::Dm => write!(f, "Dm"),
            TokenKind::Baiun => write!(f, "Baiun"),
            TokenKind::ShoutToTheVoid => write!(f, "ShoutToTheVoid"),
            TokenKind::ReturnToNature => write!(f, "ReturnToNature"),
            TokenKind::Revolve => write!(f, "Revolve"),
            TokenKind::DPrefix => write!(f, "D.["),
            TokenKind::Number(n) => write!(f, "Num(\"{}\")", n),
            TokenKind::StringLit(s) => write!(f, "Str(\"{}\")", s),
            TokenKind::Ident(id) => write!(f, "Ident(\"{}\")", id),
            TokenKind::Eq => write!(f, "Eq"),
            TokenKind::EqEq => write!(f, "EqEq"),
            TokenKind::Gte => write!(f, "Gte"),
            TokenKind::Lte => write!(f, "Lte"),
            TokenKind::Gt => write!(f, "Gt"),
            TokenKind::Lt => write!(f, "Lt"),
            TokenKind::Neq => write!(f, "Neq"),
            TokenKind::Plus => write!(f, "Plus"),
            TokenKind::Minus => write!(f, "Minus"),
            TokenKind::Star => write!(f, "Star"),
            TokenKind::Slash => write!(f, "Slash"),
            TokenKind::Percent => write!(f, "Percent"),
            TokenKind::OpenSq => write!(f, "["),
            TokenKind::CloseSq => write!(f, "]"),
            TokenKind::FullOpenSq => write!(f, "FullOpenSq(［)"),
            TokenKind::OpenBrace => write!(f, "{{"),
            TokenKind::CloseBrace => write!(f, "}}"),
            TokenKind::OpenParen => write!(f, "("),
            TokenKind::CloseParen => write!(f, ")"),
            TokenKind::FullOpenCorner => write!(f, "FullOpenCorner(【)"),
            TokenKind::FullCloseCorner => write!(f, "FullCloseCorner(】)"),
            TokenKind::Tilde => write!(f, "Tilde(～)"),
            TokenKind::MapleLeaf => write!(f, "🍁"),
            TokenKind::Semi => write!(f, ";"),
            TokenKind::FullSemi => write!(f, "FullSemi(；)"),
            TokenKind::Comma => write!(f, ","),
            TokenKind::Comment(c) => write!(f, "Comment(\"{}\")", c),
        }
    }
}

#[derive(Debug, Clone)]
pub struct Token {
    pub kind: TokenKind,
    pub line: usize,
}

pub fn tokenize_line(line: &str, line_num: usize) -> Vec<Token> {
    let trimmed = line.trim();
    if trimmed.is_empty() {
        return vec![];
    }

    if trimmed.starts_with("//eh") {
        return vec![Token {
            kind: TokenKind::Comment(trimmed.to_string()),
            line: line_num,
        }];
    }

    let chars: Vec<char> = trimmed.chars().collect();
    let mut tokens = Vec::new();
    let mut i = 0;

    while i < chars.len() {
        if chars[i].is_whitespace() {
            i += 1;
            continue;
        }

        // Full-width special chars
        match chars[i] {
            '［' => { tokens.push(tk(TokenKind::FullOpenSq, line_num)); i += 1; continue; }
            '【' => { tokens.push(tk(TokenKind::FullOpenCorner, line_num)); i += 1; continue; }
            '】' => { tokens.push(tk(TokenKind::FullCloseCorner, line_num)); i += 1; continue; }
            '～' => { tokens.push(tk(TokenKind::Tilde, line_num)); i += 1; continue; }
            '🍁' => { tokens.push(tk(TokenKind::MapleLeaf, line_num)); i += 1; continue; }
            '；' => { tokens.push(tk(TokenKind::FullSemi, line_num)); i += 1; continue; }
            _ => {}
        }

        // Comment
        if chars[i] == '/' && i + 1 < chars.len() && chars[i + 1] == '/' {
            let rest: String = chars[i..].iter().collect();
            tokens.push(Token { kind: TokenKind::Comment(rest), line: line_num });
            break;
        }

        // String literal
        if chars[i] == '"' {
            i += 1;
            let start = i;
            while i < chars.len() && chars[i] != '"' {
                if chars[i] == '\\' && i + 1 < chars.len() { i += 1; }
                i += 1;
            }
            let s: String = chars[start..i].iter().collect();
            if i < chars.len() { i += 1; }
            tokens.push(Token { kind: TokenKind::StringLit(s), line: line_num });
            continue;
        }

        // D. [ prefix
        if chars[i] == 'D' && i + 2 < chars.len() && chars[i + 1] == '.' && chars[i + 2] == ' ' {
            let mut j = i + 3;
            while j < chars.len() && chars[j] == ' ' { j += 1; }
            if j < chars.len() && chars[j] == '[' {
                tokens.push(Token { kind: TokenKind::DPrefix, line: line_num });
                i = j + 1;
                continue;
            }
        }

        // Number
        if chars[i].is_ascii_digit() {
            let start = i;
            while i < chars.len() && (chars[i].is_ascii_digit() || chars[i] == '.') { i += 1; }
            let n: String = chars[start..i].iter().collect();
            tokens.push(Token { kind: TokenKind::Number(n), line: line_num });
            continue;
        }

        // Identifier or keyword
        if chars[i] == '_' || chars[i].is_alphabetic() {
            let start = i;
            while i < chars.len() && (chars[i].is_alphanumeric() || chars[i] == '_' || chars[i] == '🍁') {
                i += 1;
            }
            let word: String = chars[start..i].iter().collect();
            let kind = match word.as_str() {
                "ganosly" if i < chars.len() && chars[i] == ':' => { i += 1; TokenKind::Ganosly }
                "Lomiior" => TokenKind::Lomiior,
                "CI" if i < chars.len() && chars[i] == ',' => { i += 1; TokenKind::CI }
                "Dm" if i < chars.len() && chars[i] == ':' => { i += 1; TokenKind::Dm }
                "baiun" if i < chars.len() && chars[i] == ':' => { i += 1; TokenKind::Baiun }
                "shoutToTheVoid" if i < chars.len() && chars[i] == ':' => { i += 1; TokenKind::ShoutToTheVoid }
                "returnToNature" if i < chars.len() && chars[i] == ':' => { i += 1; TokenKind::ReturnToNature }
                "revolve" if i < chars.len() && chars[i] == ':' => { i += 1; TokenKind::Revolve }
                _ => TokenKind::Ident(word),
            };
            tokens.push(Token { kind, line: line_num });
            continue;
        }

        // Operators
        if chars[i] == '=' && i + 1 < chars.len() && chars[i + 1] == '=' {
            tokens.push(tk(TokenKind::EqEq, line_num)); i += 2; continue;
        }
        if chars[i] == '!' && i + 1 < chars.len() && chars[i + 1] == '=' {
            tokens.push(tk(TokenKind::Neq, line_num)); i += 2; continue;
        }
        if chars[i] == '>' && i + 1 < chars.len() && chars[i + 1] == '=' {
            tokens.push(tk(TokenKind::Gte, line_num)); i += 2; continue;
        }
        if chars[i] == '<' && i + 1 < chars.len() && chars[i + 1] == '=' {
            tokens.push(tk(TokenKind::Lte, line_num)); i += 2; continue;
        }
        if chars[i] == '=' { tokens.push(tk(TokenKind::Eq, line_num)); i += 1; continue; }
        if chars[i] == '>' { tokens.push(tk(TokenKind::Gt, line_num)); i += 1; continue; }
        if chars[i] == '<' { tokens.push(tk(TokenKind::Lt, line_num)); i += 1; continue; }
        if chars[i] == '+' { tokens.push(tk(TokenKind::Plus, line_num)); i += 1; continue; }
        if chars[i] == '-' { tokens.push(tk(TokenKind::Minus, line_num)); i += 1; continue; }
        if chars[i] == '*' { tokens.push(tk(TokenKind::Star, line_num)); i += 1; continue; }
        if chars[i] == '/' { tokens.push(tk(TokenKind::Slash, line_num)); i += 1; continue; }
        if chars[i] == '%' { tokens.push(tk(TokenKind::Percent, line_num)); i += 1; continue; }

        // Delimiters
        if chars[i] == '[' { tokens.push(tk(TokenKind::OpenSq, line_num)); i += 1; continue; }
        if chars[i] == ']' { tokens.push(tk(TokenKind::CloseSq, line_num)); i += 1; continue; }
        if chars[i] == '{' { tokens.push(tk(TokenKind::OpenBrace, line_num)); i += 1; continue; }
        if chars[i] == '}' { tokens.push(tk(TokenKind::CloseBrace, line_num)); i += 1; continue; }
        if chars[i] == '(' { tokens.push(tk(TokenKind::OpenParen, line_num)); i += 1; continue; }
        if chars[i] == ')' { tokens.push(tk(TokenKind::CloseParen, line_num)); i += 1; continue; }
        if chars[i] == ';' { tokens.push(tk(TokenKind::Semi, line_num)); i += 1; continue; }
        if chars[i] == ',' { tokens.push(tk(TokenKind::Comma, line_num)); i += 1; continue; }

        i += 1;
    }
    tokens
}

fn tk(kind: TokenKind, line: usize) -> Token {
    Token { kind, line }
}

pub fn tokenize_all(lines: &[&str]) -> Vec<Vec<Token>> {
    lines.iter().enumerate().map(|(i, l)| tokenize_line(l, i + 1)).collect()
}
