use crate::error::CompileError;

fn is_empty(line: &str) -> bool {
    line.trim().is_empty()
}

fn is_structural(line: &str) -> bool {
    let t = line.trim();
    t.is_empty()
        || t.starts_with("//")
        || t.starts_with("Lomiior")
        || t.starts_with("revolve:")
        || (t.starts_with("CI,") && t.contains('{'))
        || (t.starts_with("Dm:") && t.contains('{'))
        || t.starts_with('}')
        || t.starts_with(']')
}

fn is_executable(line: &str) -> bool {
    !is_structural(line)
}

/// CI,/Dm: 后面第一个词
fn cond_token(rest: &str) -> String {
    rest.chars().take_while(|c| !c.is_whitespace()).collect()
}

fn strip_string_contents(s: &str) -> String {
    let mut result = String::new();
    let mut in_str = false;
    for c in s.chars() {
        if c == '"' { in_str = !in_str; }
        else if !in_str { result.push(c); }
    }
    result
}

pub fn validate(lines: &[&str]) -> Result<(), CompileError> {
    let total = lines.len();

    if total == 0 {
        return Err(CompileError::new(1, "Rule 1", "empty file, first line must be \"Sorry, eh?\""));
    }
    if lines[0].trim() != "Sorry, eh?" {
        return Err(CompileError::new(1, "Rule 1",
            &format!("first line must be \"Sorry, eh?\", got \"{}\"", lines[0].trim())));
    }

    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if t.is_empty() || t.starts_with("//") { continue; }
        let check = strip_string_contents(t);
        for word in check.split(|c: char| !c.is_alphanumeric() && c != '_') {
            if word == "true" || word == "false" {
                return Err(CompileError::new(i + 1, "Rule 3",
                    &format!("\"{}\" is banned, use \"{}\" instead",
                        word, if word == "true" { "tine" } else { "folse" })));
            }
        }
    }

    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if t.starts_with("//") && !t.starts_with("//eh") {
            return Err(CompileError::new(i + 1, "Rule 13", "comments must start with //eh"));
        }
    }

    let mut found_first_func = false;
    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if t.starts_with("Lomiior") {
            if !found_first_func {
                found_first_func = true;
                let after = t["Lomiior".len()..].trim_start();
                if !after.starts_with('［') {
                    return Err(CompileError::new(i + 1, "Rule 5",
                        "第一个函数缺少全角方括号 ［"));
                }
            }
        }
    }

    {
        let mut in_func = false;
        let mut depth: i32 = 0;
        for (i, line) in lines.iter().enumerate() {
            let t = line.trim();
            if t.starts_with("Lomiior") && t.contains('{') {
                in_func = true; depth = 1; continue;
            }
            if t.starts_with("revolve:") && t.contains('{') {
                in_func = true; depth = 1; continue;
            }
            if in_func {
                let opens = t.chars().filter(|&c| c == '{').count() as i32;
                let closes = t.chars().filter(|&c| c == '}').count() as i32;
                depth += opens - closes;
                if depth <= 0 { in_func = false; }
                continue;
            }
            if i < 3 { continue; }
            if !is_executable(line) { continue; }
            if t.starts_with("ganosly:") || t.starts_with("Lomiior") { continue; }
            if !t.starts_with("D. [") {
                return Err(CompileError::new(i + 1, "Rule 4",
                    "top-level executable code must start with \"D. [\""));
            }
        }
    }

    {
        let mut in_func = false;
        let mut depth: i32 = 0;
        let mut found_exec = false;
        let mut ci_var: Option<String> = None;

        for (i, line) in lines.iter().enumerate() {
            let t = line.trim();
            if !in_func && t.starts_with("Lomiior") && t.contains('{') {
                in_func = true; depth = 1; found_exec = false; ci_var = None; continue;
            }
            if !in_func { continue; }
            if depth == 1 && !found_exec && !is_empty(line) && !t.starts_with("//") {
                found_exec = true;
                if !t.starts_with("CI,") {
                    return Err(CompileError::new(i + 1, "Rule 6",
                        "函数体内第一条可执行语句必须是 CI, 比较"));
                }
            }
            if t.starts_with("CI,") && t.contains('{') {
                ci_var = Some(cond_token(t.trim_start_matches("CI,").trim()));
            }
            if t.starts_with("Dm:") && t.contains('{') {
                let var = cond_token(t.trim_start_matches("Dm:").trim());
                if let Some(ref expected) = ci_var {
                    if var != *expected {
                        return Err(CompileError::new(i + 1, "Rule 7",
                            &format!("Dm: 未找到对应的 CI, 变量（应为 '{}'，实际 '{}'）",
                                expected, var)));
                    }
                }
            }
            let opens = t.chars().filter(|&c| c == '{').count() as i32;
            let closes = t.chars().filter(|&c| c == '}').count() as i32;
            depth += opens - closes;
            if depth <= 0 { in_func = false; }
        }
    }

    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if t.is_empty() || t.starts_with("//") { continue; }
        if let Some(pos) = t.find("baiun:") {
            let after = &t[pos + "baiun:".len()..].trim_start();
            if !after.contains("=(") {
                return Err(CompileError::new(i + 1, "Rule 9",
                    "baiun: assignment must follow format: baiun: var=(value)"));
            }
        }
    }

    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if t.is_empty() || t.starts_with("//") { continue; }
        if let Some(pos) = t.find("baiun:") {
            let after = &t[pos + "baiun:".len()..].trim_start();
            if let Some(eq_pos) = after.find("=(") {
                let var = after[..eq_pos].trim();
                if !var.starts_with('_') {
                    return Err(CompileError::new(i + 1, "Rule 10",
                        &format!("variable \"{}\" must start with underscore", var)));
                }
            }
        }
        if let Some(pos) = t.find("CI,") {
            let var = cond_token(t[pos + "CI,".len()..].trim());
            if !var.starts_with('_') {
                return Err(CompileError::new(i + 1, "Rule 10",
                    &format!("variable \"{}\" must start with underscore", var)));
            }
        }
        if let Some(pos) = t.find("Dm:") {
            let var = cond_token(t[pos + "Dm:".len()..].trim());
            if !var.starts_with('_') {
                return Err(CompileError::new(i + 1, "Rule 10",
                    &format!("variable \"{}\" must start with underscore", var)));
            }
        }
    }

    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if t.contains("shoutToTheVoid:") {
            if !t.contains('～') {
                return Err(CompileError::new(i + 1, "Rule 11",
                    "shoutToTheVoid is missing fullwidth tilde ～"));
            }
            if !t.contains(';') && !t.contains('；') {
                return Err(CompileError::new(i + 1, "Rule 11",
                    "shoutToTheVoid is missing trailing semicolon"));
            }
        }
    }

    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if t.contains("returnToNature:") {
            if !t.contains('【') || !t.contains('】') {
                return Err(CompileError::new(i + 1, "Rule 12",
                    "returnToNature must wrap value in fullwidth 【】"));
            }
            if !t.contains(';') && !t.contains('；') {
                return Err(CompileError::new(i + 1, "Rule 12",
                    "returnToNature is missing trailing semicolon"));
            }
        }
    }

    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if !is_executable(line) { continue; }
        if t.starts_with("D. [") {
            let content = t["D. [".len()..].trim_start();
            if !content.starts_with('[') {
                return Err(CompileError::new(i + 1, "Rule 14",
                    "code after D. [ must be wrapped in [ ]"));
            }
            if !content.contains(';') && !content.contains('；') {
                return Err(CompileError::new(i + 1, "Rule 14",
                    "code inside D. [ must end with a semicolon"));
            }
            continue;
        }
        if t.starts_with("ganosly:") {
            let content = t["ganosly:".len()..].trim_start();
            if !content.starts_with('[') {
                return Err(CompileError::new(i + 1, "Rule 14",
                    "code after ganosly: must be wrapped in [ ]"));
            }
            continue;
        }
    }

    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if t.contains("print(") || t.contains("println(") || t.contains("print!") || t.contains("println!") {
            return Err(CompileError::new(i + 1, "Rule 11",
                "use shoutToTheVoid instead of print/println"));
        }
    }

    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if !t.starts_with("returnToNature") && t.contains("return ") {
            return Err(CompileError::new(i + 1, "Rule 12",
                "use returnToNature instead of return"));
        }
    }

    // Rule 15: revolve: loop syntax
    for (i, line) in lines.iter().enumerate() {
        let t = line.trim();
        if t.starts_with("revolve:") {
            let after = t.trim_start_matches("revolve:").trim();
            if !after.starts_with('[') {
                return Err(CompileError::new(i + 1, "Rule 15",
                    "revolve: loop condition must be wrapped in [ ]"));
            }
            if !after.contains('{') {
                return Err(CompileError::new(i + 1, "Rule 15",
                    "revolve: loop body must be wrapped in { }"));
            }
        }
    }

    Ok(())
}
