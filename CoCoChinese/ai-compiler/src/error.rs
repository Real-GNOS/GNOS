use std::fmt;

#[derive(Debug)]
pub struct CompileError {
    pub line: usize,
    pub rule: String,
    pub message: String,
}

impl fmt::Display for CompileError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "\x1b[31m❌ Error at line {} [{}]: {}\x1b[0m",
            self.line, self.rule, self.message
        )
    }
}

impl CompileError {
    pub fn new(line: usize, rule: &str, message: &str) -> Self {
        Self {
            line,
            rule: rule.to_string(),
            message: message.to_string(),
        }
    }
}
