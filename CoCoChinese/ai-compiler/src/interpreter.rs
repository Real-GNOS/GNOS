use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

use crate::error::CompileError;

/// 默认的递归深度保护（可以用 AIFOLIA_MAX_DEPTH 覆盖）。
/// 注意：这只限制"函数递归深度"，不限制循环次数——图灵完备靠的是无界 revolve: 循环。
/// 40000 是实测出来的安全值：解释器跑在 256MB 大栈线程里（AIFOLIA_STACK_MB），
/// 140000 层会爆原生栈，40000 层能优雅报错、50000 层能正常跑完。
const DEFAULT_MAX_DEPTH: usize = 40_000;

#[derive(Debug, Clone)]
pub enum Value {
    Num(f64),
    Str(String),
    Bool(bool),
    /// 数组：Rc<RefCell<..>> —— 克隆是廉价的，且可以被 push/set 原地改大改小（无界存储）
    List(Rc<RefCell<Vec<Value>>>),
    Void,
}

impl Value {
    pub fn list(items: Vec<Value>) -> Value {
        Value::List(Rc::new(RefCell::new(items)))
    }

    pub fn truthy(&self) -> bool {
        match self {
            Value::Bool(b) => *b,
            Value::Num(n) => *n != 0.0,
            Value::Str(s) => !s.is_empty(),
            Value::List(l) => !l.borrow().is_empty(),
            Value::Void => false,
        }
    }

    pub fn type_name(&self) -> &'static str {
        match self {
            Value::Num(_) => "number",
            Value::Str(_) => "string",
            Value::Bool(_) => "bool",
            Value::List(_) => "list",
            Value::Void => "void",
        }
    }

    pub fn as_num(&self) -> f64 {
        match self {
            Value::Num(n) => *n,
            Value::Bool(b) => if *b { 1.0 } else { 0.0 },
            Value::Str(s) => s.trim().parse::<f64>().unwrap_or(0.0),
            _ => 0.0,
        }
    }
}

impl std::fmt::Display for Value {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Value::Num(n) => {
                if *n == (*n as i64) as f64 {
                    write!(f, "{}", *n as i64)
                } else {
                    write!(f, "{}", n)
                }
            }
            Value::Str(s) => write!(f, "{}", s),
            Value::Bool(b) => write!(f, "{}", if *b { "tine" } else { "folse" }),
            Value::List(items) => {
                let parts: Vec<String> = items.borrow().iter().map(|v| v.to_string()).collect();
                write!(f, "[{}]", parts.join(", "))
            }
            Value::Void => write!(f, "()"),
        }
    }
}

#[derive(Clone)]
struct FuncDef {
    params: Vec<String>,
    body_start: usize,
    body_end: usize,
}

/// 语句执行结果：函数里遇到 returnToNature: 就往上冒泡
enum Flow {
    Normal,
    Return(Value),
}

pub struct Interpreter {
    globals: HashMap<String, Value>,
    functions: HashMap<String, FuncDef>,
    output: Vec<String>,
    lines: Vec<String>,
    /// Some(n) = 循环次数上限（安全阀，默认 None 表示无上限）
    max_iter: Option<u64>,
    max_depth: usize,
    depth: usize,
}

impl Interpreter {
    pub fn new() -> Self {
        Self::with_limits(env_limit("AIFOLIA_MAX_ITER"), env_limit("AIFOLIA_MAX_DEPTH").map(|n| n as usize))
    }

    pub fn with_limits(max_iter: Option<u64>, max_depth: Option<usize>) -> Self {
        Self {
            globals: HashMap::new(),
            functions: HashMap::new(),
            output: Vec::new(),
            lines: Vec::new(),
            max_iter,
            max_depth: max_depth.unwrap_or(DEFAULT_MAX_DEPTH),
            depth: 0,
        }
    }

    pub fn run(&mut self, lines: &[&str]) -> Result<Vec<String>, CompileError> {
        self.lines = lines.iter().map(|s| s.to_string()).collect();
        self.collect_functions()?;
        let total = self.lines.len();
        let _ = self.exec_block(0, total)?;
        Ok(self.output.clone())
    }

    fn line(&self, i: usize) -> &str {
        self.lines[i].trim()
    }

    /// 找到第 open_line 行那个 `{` 对应的 `}` 所在行（字符串和注释里的花括号不算）
    fn block_close(&self, open_line: usize) -> Result<usize, CompileError> {
        let mut depth: i32 = 0;
        for j in open_line..self.lines.len() {
            let (o, c) = scan_braces(&self.lines[j]);
            depth += o - c;
            if depth <= 0 {
                return Ok(j);
            }
        }
        Err(CompileError::new(open_line + 1, "Syntax", "unclosed '{' block"))
    }

    /// 跳过空行与注释行
    fn next_code_line(&self, from: usize, end: usize) -> Option<usize> {
        let mut i = from;
        while i < end {
            let t = self.line(i);
            if !t.is_empty() && !t.starts_with("//") {
                return Some(i);
            }
            i += 1;
        }
        None
    }

    fn collect_functions(&mut self) -> Result<(), CompileError> {
        let mut i = 0;
        while i < self.lines.len() {
            let raw = self.line(i).to_string();
            if raw.starts_with("Lomiior") {
                let after = raw["Lomiior".len()..].trim_start();
                let after = after.trim_start_matches('［').trim_start_matches('[');
                let name: String = after.chars().take_while(|c| *c != '(' && *c != '（').collect();
                let name = name.trim().to_string();

                let open = after.find('(').or_else(|| after.find('（'));
                let close = after.rfind(')').or_else(|| after.rfind('）'));
                let params = match (open, close) {
                    (Some(o), Some(c)) if c > o => after[o + 1..c]
                        .split(',')
                        .map(|p| p.trim().trim_start_matches('【').trim_end_matches('】').to_string())
                        .filter(|p| !p.is_empty())
                        .collect(),
                    _ => vec![],
                };

                if name.is_empty() {
                    return Err(CompileError::new(i + 1, "Syntax", "Lomiior needs a function name"));
                }

                let close_line = self.block_close(i)?;
                self.functions.insert(
                    name,
                    FuncDef { params, body_start: i + 1, body_end: close_line },
                );
                i = close_line + 1;
                continue;
            }
            i += 1;
        }
        Ok(())
    }

    /// 统一的语句块执行器：[start, end) 是行区间。
    /// 顶层、函数体、if/else 体、循环体，全都走这里 —— 所以
    ///   · `D. [ [ ... ]; ] ]` 包裹的语句在函数体里也能真正执行（原来是静默吞掉）
    ///   · returnToNature: 能从任意深度冒泡出去
    fn exec_block(&mut self, start: usize, end: usize) -> Result<Flow, CompileError> {
        let mut i = start;
        while i < end {
            let raw = self.line(i).to_string();
            if raw.is_empty() || raw.starts_with("//") {
                i += 1;
                continue;
            }

            // ---- 条件：CI, <条件> { ... }  后面可选 Dm: <条件> { ... } 作为 else ----
            if let Some(rest) = raw.strip_prefix("CI,") {
                let has_brace = raw.contains('{');

                if !has_brace {
                    // 兼容老写法：CI, _v 后面不带花括号时，只跑下一行
                    let cond = self.eval_condition(rest, i)?;
                    let next = self.next_code_line(i + 1, end);
                    if cond {
                        if let Some(n) = next {
                            if let Flow::Return(v) = self.exec_block(n, n + 1)? {
                                return Ok(Flow::Return(v));
                            }
                        }
                    }
                    i = next.map(|n| n + 1).unwrap_or(i + 1);
                    continue;
                }

                let close = self.block_close(i)?;

                // 收集紧跟其后的连续 Dm: 块，组成 if / else if … / else 链
                let mut chain: Vec<(usize, usize)> = Vec::new();
                let mut cursor = close + 1;
                while let Some(k) = self.next_code_line(cursor, end) {
                    if !self.line(k).starts_with("Dm:") {
                        break;
                    }
                    let kc = self.block_close(k)?;
                    chain.push((k, kc));
                    cursor = kc + 1;
                }

                let mut chosen: Option<(usize, usize)> = None;
                if self.eval_condition(rest, i)? {
                    chosen = Some((i, close));
                } else {
                    for (k, kc) in &chain {
                        let drest = self.line(*k).trim_start_matches("Dm:").to_string();
                        match self.eval_else_condition(&drest)? {
                            Some(true) => {
                                chosen = Some((*k, *kc)); // 命中（无条件 else 或条件为真）
                                break;
                            }
                            // 条件为假 → 继续看下一个 else if（这里必须是 continue，写成 break 会把整条链截断）
                            Some(false) | None => continue,
                        }
                    }
                }

                if let Some((open, oc)) = chosen {
                    if let Flow::Return(v) = self.exec_block(open + 1, oc)? {
                        return Ok(Flow::Return(v));
                    }
                }

                // 互斥：整条链只跑一支（原来 else 会无条件补跑一次）
                let after = chain.last().map(|(_, kc)| *kc).unwrap_or(close) + 1;
                i = after;
                continue;
            }

            if raw.starts_with("Dm:") {
                // 没有配对 CI 的裸 Dm: 当作普通块执行（保持向后兼容）
                let (body_end, next) = if raw.contains('{') {
                    let close = self.block_close(i)?;
                    (close, close + 1)
                } else {
                    let e = self.next_code_line(i + 1, end).map(|n| n + 1).unwrap_or(i + 1);
                    (e, e)
                };
                if let Flow::Return(v) = self.exec_block(i + 1, body_end)? {
                    return Ok(Flow::Return(v));
                }
                i = next;
                continue;
            }

            // ---- ganosly: [[ var=(value) ]] 顶层定义（老语法，保留）----
            if let Some(rest) = raw.strip_prefix("ganosly:") {
                self.exec_ganosly(rest)?;
                i += 1;
                continue;
            }

            // ---- 循环：revolve: [条件] { ... } ----
            if raw.starts_with("revolve:") {
                let after = raw.trim_start_matches("revolve:").trim();
                if !after.starts_with('[') {
                    return Err(CompileError::new(i + 1, "Rule 15", "revolve: loop condition must be wrapped in [ ]"));
                }
                let cond_src = extract_bracket_content(after).to_string();
                let close = self.block_close(i)?;
                let mut iters: u64 = 0;
                loop {
                    if let Some(max) = self.max_iter {
                        if iters >= max {
                            return Err(CompileError::new(i + 1, "Runtime",
                                &format!("revolve: loop stopped by AIFOLIA_MAX_ITER={} (safety valve, unset it for unlimited)", max)));
                        }
                    }
                    let cond = self.eval_expr(&cond_src)?;
                    if !cond.truthy() {
                        break;
                    }
                    iters += 1;
                    if let Flow::Return(v) = self.exec_block(i + 1, close)? {
                        return Ok(Flow::Return(v));
                    }
                }
                i = close + 1;
                continue;
            }

            // 函数定义块：collect_functions 已经登记过了，这里整块跳过
            if raw.starts_with("Lomiior") {
                let close = self.block_close(i)?;
                i = close + 1;
                continue;
            }

            // 头部固定行
            if unwrap_stmt(&raw) == "Sorry, eh?" {
                i += 1;
                continue;
            }

            // ---- 普通语句（可能被 D. [ [ ... ]; ] ] 包着）----
            let t = unwrap_stmt(&raw);
            if t.is_empty() {
                i += 1;
                continue;
            }

            if let Some(rest) = t.strip_prefix("returnToNature:") {
                let expr = trim_corner(rest);
                let v = if expr.is_empty() { Value::Void } else { self.eval_expr(&expr)? };
                return Ok(Flow::Return(v));
            }

            if let Some(rest) = t.strip_prefix("baiun:") {
                self.exec_baiun(rest.trim())?;
                i += 1;
                continue;
            }

            if let Some(rest) = t.strip_prefix("shoutToTheVoid:") {
                // 交给 eval_expr 处理「...」的内插
                let e = trim_semi(rest.trim_start().trim_start_matches('～'));
                let v = self.eval_expr(e)?;
                self.output.push(v.to_string());
                i += 1;
                continue;
            }

            // 表达式语句（push(...) / set(...) / foo(...)），结果丢弃
            if t.contains('(') {
                self.eval_expr(&t)?;
                i += 1;
                continue;
            }

            // 认不出来的语句：直接报错，别静默吞掉（静默吞掉正是老版本最大的坑）
            return Err(CompileError::new(i + 1, "Syntax",
                &format!("unrecognized statement: \"{}\"", t)));
        }
        Ok(Flow::Normal)
    }

    /// CI, <条件> —— 裸变量（老写法）取变量真值；其它情况按表达式求值
    fn eval_condition(&mut self, rest: &str, _line_no: usize) -> Result<bool, CompileError> {
        let mut s = rest.trim();
        if let Some(p) = s.rfind('{') {
            s = s[..p].trim();
        }
        if s.is_empty() {
            return Ok(false);
        }
        if is_bare_var(s) {
            let v = self.globals.get(s).cloned().unwrap_or(Value::Bool(false));
            return Ok(v.truthy());
        }
        Ok(self.eval_expr(s)?.truthy())
    }

    /// ganosly: [[ _x=(value) ]] —— 老语法里的顶层定义，语义等同于 baiun:
    /// Dm: 的判定：
    ///   `Dm: {`（空）或 `Dm: _x {`（裸变量）→ 无条件 else（经典写法）
    ///   `Dm: (表达式) {`            → else if：为真返回 Some(true)，为假返回 None 继续下一个
    fn eval_else_condition(&mut self, rest: &str) -> Result<Option<bool>, CompileError> {
        let mut s = rest.trim();
        if let Some(p) = s.rfind('{') {
            s = s[..p].trim();
        }
        if s.is_empty() || is_bare_var(s) {
            return Ok(Some(true));
        }
        if self.eval_expr(s)?.truthy() {
            Ok(Some(true))
        } else {
            Ok(None)
        }
    }

    fn exec_ganosly(&mut self, rest: &str) -> Result<(), CompileError> {
        let content = extract_bracket_content(rest.trim());
        self.exec_baiun(content.trim())
    }

    fn exec_baiun(&mut self, rest: &str) -> Result<(), CompileError> {
        // 支持 baiun: _x=(expr) 与 baiun: _x=expr 两种写法
        let (name, expr) = if let Some(p) = rest.find("=(") {
            let name = rest[..p].trim().to_string();
            let inner = &rest[p + 2..];
            (name, match_close_paren(inner))
        } else if let Some(p) = rest.find('=') {
            (rest[..p].trim().to_string(), rest[p + 1..].trim().to_string())
        } else {
            return Err(CompileError::new(0, "Syntax",
                &format!("baiun: assignment must look like: baiun: _x=(value), got \"{}\"", rest)));
        };
        if name.is_empty() {
            return Err(CompileError::new(0, "Syntax", "baiun: missing variable name"));
        }
        let val = self.eval_expr(&expr)?;
        self.globals.insert(name, val);
        Ok(())
    }

    fn call_function(&mut self, name: &str, args: Vec<Value>) -> Result<Value, CompileError> {
        let func = match self.functions.get(name) {
            Some(f) => f.clone(),
            None => {
                return Err(CompileError::new(0, "Runtime", &format!("undefined function '{}'", name)))
            }
        };

        if self.depth >= self.max_depth {
            return Err(CompileError::new(0, "Runtime",
                &format!("call depth exceeded AIFOLIA_MAX_DEPTH={} (raise it if you really need deeper recursion)", self.max_depth)));
        }

        let saved = self.globals.clone();
        for (param, arg) in func.params.iter().zip(args) {
            self.globals.insert(param.clone(), arg);
        }

        self.depth += 1;
        let flow = self.exec_block(func.body_start, func.body_end);
        self.depth -= 1;

        self.globals = saved;

        match flow? {
            Flow::Return(v) => Ok(v),
            Flow::Normal => Ok(Value::Void),
        }
    }

    /// 内建函数（参数以原始文本传入，push/set 需要拿到变量名才能原地改）
    fn call_builtin(&mut self, name: &str, args_str: &str) -> Result<Option<Value>, CompileError> {
        let raw_args = split_top_commas(args_str);

        match name {
            "list" => {
                let mut items = Vec::new();
                for a in &raw_args {
                    if a.trim().is_empty() {
                        continue;
                    }
                    items.push(self.eval_expr(a)?);
                }
                Ok(Some(Value::list(items)))
            }
            "len" => {
                let v = self.eval_one(&raw_args, "len")?;
                let n = match &v {
                    Value::List(l) => l.borrow().len(),
                    Value::Str(s) => s.chars().count(),
                    _ => return Err(CompileError::new(0, "Runtime",
                        &format!("len() needs a list or string, got {}", v.type_name()))),
                };
                Ok(Some(Value::Num(n as f64)))
            }
            "push" => {
                let target = self.arg_lvalue(&raw_args, "push")?;
                let v = self.eval_expr(nth(&raw_args, 1).ok_or_else(|| {
                    CompileError::new(0, "Runtime", "push() needs 2 args: push(_list, value)")
                })?)?;
                let list = self.get_or_make_list(&target)?;
                list.borrow_mut().push(v);
                Ok(Some(Value::List(list)))
            }
            "pop" => {
                let target = self.arg_lvalue(&raw_args, "pop")?;
                let list = self.get_or_make_list(&target)?;
                let v = list.borrow_mut().pop().unwrap_or(Value::Void);
                Ok(Some(v))
            }
            "set" => {
                let target = self.arg_lvalue(&raw_args, "set")?;
                let idx_val = self.eval_expr(nth(&raw_args, 1).ok_or_else(|| {
                    CompileError::new(0, "Runtime", "set() needs 3 args: set(_list, index, value)")
                })?)?;
                let v = self.eval_expr(nth(&raw_args, 2).ok_or_else(|| {
                    CompileError::new(0, "Runtime", "set() needs 3 args: set(_list, index, value)")
                })?)?;
                let list = self.get_or_make_list(&target)?;
                let len = list.borrow().len();
                let i = to_index(&idx_val, len)?;
                list.borrow_mut()[i] = v.clone();
                Ok(Some(v))
            }
            "str" => {
                let v = self.eval_one(&raw_args, "str")?;
                Ok(Some(Value::Str(v.to_string())))
            }
            "num" => {
                let v = self.eval_one(&raw_args, "num")?;
                match &v {
                    Value::Num(_) => Ok(Some(v)),
                    Value::Str(s) => match s.trim().parse::<f64>() {
                        Ok(n) => Ok(Some(Value::Num(n))),
                        Err(_) => Err(CompileError::new(0, "Runtime",
                            &format!("num() cannot parse \"{}\"", s))),
                    },
                    _ => Ok(Some(Value::Num(v.as_num()))),
                }
            }
            _ => Ok(None),
        }
    }

    fn eval_one(&mut self, args: &[String], who: &str) -> Result<Value, CompileError> {
        let a = nth(args, 0).ok_or_else(|| {
            CompileError::new(0, "Runtime", &format!("{}() needs 1 arg", who))
        })?;
        self.eval_expr(a)
    }

    fn arg_lvalue(&self, args: &[String], who: &str) -> Result<String, CompileError> {
        let a = nth(args, 0).ok_or_else(|| {
            CompileError::new(0, "Runtime", &format!("{}() needs a variable as its first arg", who))
        })?;
        let name = a.trim().to_string();
        if !is_bare_var(&name) {
            return Err(CompileError::new(0, "Runtime",
                &format!("{}() first arg must be a variable name (like _xs), got \"{}\"", who, name)));
        }
        Ok(name)
    }

    fn get_or_make_list(&mut self, name: &str) -> Result<Rc<RefCell<Vec<Value>>>, CompileError> {
        match self.globals.get(name).cloned() {
            Some(Value::List(l)) => Ok(l),
            Some(other) => Err(CompileError::new(0, "Runtime",
                &format!("'{}' is a {}, not a list", name, other.type_name()))),
            None => {
                let l: Rc<RefCell<Vec<Value>>> = Rc::new(RefCell::new(Vec::new()));
                self.globals.insert(name.to_string(), Value::List(l.clone()));
                Ok(l)
            }
        }
    }

    /// _xs(i) 取值 / _xs(i, v) 赋值；字符串也能按字符下标取
    fn index_value(&mut self, name: &str, args_str: &str) -> Result<Value, CompileError> {
        let args = split_top_commas(args_str);
        let idx_val = self.eval_expr(nth(&args, 0).ok_or_else(|| {
            CompileError::new(0, "Runtime", &format!("{}() needs an index", name))
        })?)?;

        let cur = self.globals.get(name).cloned().unwrap_or(Value::Void);
        match cur {
            Value::List(l) => {
                let len = l.borrow().len();
                let i = to_index(&idx_val, len)?;
                if args.len() >= 2 {
                    let v = self.eval_expr(&args[1])?;
                    l.borrow_mut()[i] = v.clone();
                    Ok(v)
                } else {
                    Ok(l.borrow().get(i).cloned().unwrap_or(Value::Void))
                }
            }
            Value::Str(s) => {
                let chars: Vec<char> = s.chars().collect();
                let i = to_index(&idx_val, chars.len())?;
                Ok(Value::Str(chars[i].to_string()))
            }
            other => Err(CompileError::new(0, "Runtime",
                &format!("'{}' is a {}, cannot index it", name, other.type_name()))),
        }
    }

    fn eval_expr(&mut self, expr: &str) -> Result<Value, CompileError> {
        let expr = expr.trim();

        // 「字符串」/【字符串】：内插
        if (expr.starts_with('「') && expr.ends_with('」')) || (expr.starts_with('【') && expr.ends_with('】')) {
            let inner = &expr[3..expr.len() - 3];
            return Ok(Value::Str(self.interpolate(inner)?));
        }

        if expr.is_empty() {
            return Ok(Value::Void);
        }

        // 括号分组：(expr)
        if expr.starts_with('(') && expr.ends_with(')') {
            return self.eval_expr(&expr[1..expr.len() - 1]);
        }

        // 数组字面量：[1, 2, _x]
        if expr.starts_with('[') && expr.ends_with(']') {
            let inner = &expr[1..expr.len() - 1];
            let mut items = Vec::new();
            for part in split_top_commas(inner) {
                if part.trim().is_empty() {
                    continue;
                }
                items.push(self.eval_expr(&part)?);
            }
            return Ok(Value::list(items));
        }

        if expr.starts_with('"') && expr.ends_with('"') && expr.len() >= 2 {
            let inner = &expr[1..expr.len() - 1];
            return Ok(Value::Str(self.interpolate(inner)?));
        }
        if expr == "tine" {
            return Ok(Value::Bool(true));
        }
        if expr == "folse" {
            return Ok(Value::Bool(false));
        }

        if let Ok(n) = expr.parse::<i64>() {
            return Ok(Value::Num(n as f64));
        }
        if let Ok(n) = expr.parse::<f64>() {
            return Ok(Value::Num(n));
        }

        if let Some(val) = self.globals.get(expr) {
            return Ok(val.clone());
        }

        if let Some(pos) = find_op(expr, "!=") {
            let l = self.eval_expr(&expr[..pos])?;
            let r = self.eval_expr(&expr[pos + 2..])?;
            return Ok(Value::Bool(!self.values_equal(&l, &r)));
        }
        if let Some(pos) = find_op(expr, "==") {
            let l = self.eval_expr(&expr[..pos])?;
            let r = self.eval_expr(&expr[pos + 2..])?;
            return Ok(Value::Bool(self.values_equal(&l, &r)));
        }
        if let Some(pos) = find_op(expr, ">=") {
            let l = self.eval_expr(&expr[..pos])?;
            let r = self.eval_expr(&expr[pos + 2..])?;
            return Ok(Value::Bool(l.as_num() >= r.as_num()));
        }
        if let Some(pos) = find_op(expr, "<=") {
            let l = self.eval_expr(&expr[..pos])?;
            let r = self.eval_expr(&expr[pos + 2..])?;
            return Ok(Value::Bool(l.as_num() <= r.as_num()));
        }
        if let Some(pos) = find_op_char(expr, '>') {
            let l = self.eval_expr(&expr[..pos])?;
            let r = self.eval_expr(&expr[pos + 1..])?;
            return Ok(Value::Bool(l.as_num() > r.as_num()));
        }
        if let Some(pos) = find_op_char(expr, '<') {
            let l = self.eval_expr(&expr[..pos])?;
            let r = self.eval_expr(&expr[pos + 1..])?;
            return Ok(Value::Bool(l.as_num() < r.as_num()));
        }

        if let Some(pos) = find_op_char(expr, '+') {
            let l = self.eval_expr(&expr[..pos])?;
            let r = self.eval_expr(&expr[pos + 1..])?;
            return match (&l, &r) {
                (Value::Num(a), Value::Num(b)) => Ok(Value::Num(a + b)),
                (Value::List(a), Value::List(b)) => {
                    let mut out = a.borrow().clone();
                    out.extend(b.borrow().iter().cloned());
                    Ok(Value::list(out))
                }
                (Value::Str(a), b) => Ok(Value::Str(format!("{}{}", a, b))),
                (a, Value::Str(b)) => Ok(Value::Str(format!("{}{}", a, b))),
                _ => Ok(Value::Num(l.as_num() + r.as_num())),
            };
        }
        if let Some(pos) = find_op_char(expr, '-') {
            let l = self.eval_expr(&expr[..pos])?;
            let r = self.eval_expr(&expr[pos + 1..])?;
            return Ok(Value::Num(l.as_num() - r.as_num()));
        }
        if let Some(pos) = find_op_char(expr, '*') {
            let l = self.eval_expr(&expr[..pos])?;
            let r = self.eval_expr(&expr[pos + 1..])?;
            return Ok(Value::Num(l.as_num() * r.as_num()));
        }
        if let Some(pos) = find_op_char(expr, '/') {
            let l = self.eval_expr(&expr[..pos])?;
            let r = self.eval_expr(&expr[pos + 1..])?;
            let d = r.as_num();
            if d == 0.0 {
                return Err(CompileError::new(0, "Runtime", "division by zero"));
            }
            return Ok(Value::Num(l.as_num() / d));
        }
        if let Some(pos) = find_op_char(expr, '%') {
            let l = self.eval_expr(&expr[..pos])?;
            let r = self.eval_expr(&expr[pos + 1..])?;
            let d = r.as_num();
            if d == 0.0 {
                return Err(CompileError::new(0, "Runtime", "modulo by zero"));
            }
            return Ok(Value::Num(l.as_num() % d));
        }

        // 调用 / 下标：name(args)
        if let Some((callee, args_str)) = split_call(expr) {
            if !callee.is_empty() {
                if self.functions.contains_key(&callee) {
                    let args = self.eval_args(args_str)?;
                    return self.call_function(&callee, args);
                }
                if let Some(v) = self.call_builtin(&callee, args_str)? {
                    return Ok(v);
                }
                if self.globals.contains_key(&callee) {
                    return self.index_value(&callee, args_str);
                }
                let args = self.eval_args(args_str)?;
                return self.call_function(&callee, args);
            }
        }

        // 裸标识符兜底：查不到就当字符串字面量（保持老行为）
        Ok(Value::Str(expr.to_string()))
    }

    fn eval_args(&mut self, args_str: &str) -> Result<Vec<Value>, CompileError> {
        let mut out = Vec::new();
        for part in split_top_commas(args_str) {
            if part.trim().is_empty() {
                continue;
            }
            out.push(self.eval_expr(&part)?);
        }
        Ok(out)
    }

    fn values_equal(&self, a: &Value, b: &Value) -> bool {
        match (a, b) {
            (Value::Num(x), Value::Num(y)) => (x - y).abs() < f64::EPSILON,
            (Value::Str(x), Value::Str(y)) => x == y,
            (Value::Bool(x), Value::Bool(y)) => x == y,
            (Value::Void, Value::Void) => true,
            (Value::List(x), Value::List(y)) => {
                if Rc::ptr_eq(x, y) {
                    return true;
                }
                let (xb, yb) = (x.borrow(), y.borrow());
                if xb.len() != yb.len() {
                    return false;
                }
                xb.iter().zip(yb.iter()).all(|(p, q)| self.values_equal(p, q))
            }
            _ => false,
        }
    }

    fn interpolate(&self, s: &str) -> Result<String, CompileError> {
        let mut result = String::new();
        let mut chars = s.chars().peekable();
        while let Some(c) = chars.next() {
            if c == '_' {
                let mut var = String::new();
                var.push('_');
                while let Some(&next) = chars.peek() {
                    if next.is_alphanumeric() || next == '_' || next == '🍁' {
                        var.push(next);
                        chars.next();
                    } else {
                        break;
                    }
                }
                if let Some(val) = self.globals.get(&var) {
                    result.push_str(&val.to_string());
                } else {
                    result.push_str(&var);
                }
            } else {
                result.push(c);
            }
        }
        Ok(result)
    }
}

/* ============================ 解析小工具 ============================ */

fn env_limit(key: &str) -> Option<u64> {
    std::env::var(key).ok().and_then(|v| v.trim().parse::<u64>().ok()).filter(|n| *n > 0)
}

/// 花括号计数：忽略字符串里的、以及 `//` 之后的（注释里的括号不该影响块结构）
fn scan_braces(s: &str) -> (i32, i32) {
    let chars: Vec<char> = s.chars().collect();
    let mut opens = 0;
    let mut closes = 0;
    let mut in_str = false;
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        if c == '"' {
            in_str = !in_str;
        } else if !in_str {
            if c == '/' && i + 1 < chars.len() && chars[i + 1] == '/' {
                break;
            }
            if c == '{' {
                opens += 1;
            }
            if c == '}' {
                closes += 1;
            }
        }
        i += 1;
    }
    (opens, closes)
}

/// 把 `D. [ [stmt]; ] ]` 还原成 `stmt`
pub fn unwrap_stmt(line: &str) -> String {
    let t = line.trim();
    let body = if let Some(rest) = t.strip_prefix("D.[") {
        Some(rest.trim_start())
    } else {
        t.strip_prefix("D. [").map(|r| r.trim_start())
    };
    let content = match body {
        Some(rest) => {
            let mut c = extract_bracket_content(rest).to_string();
            if c.starts_with('[') {
                c = extract_bracket_content(&c).to_string();
            }
            c
        }
        None => t.to_string(),
    };
    trim_semi(&content).to_string()
}

fn trim_semi(s: &str) -> &str {
    s.trim().trim_end_matches(';').trim_end_matches('；').trim()
}

/// 去掉 returnToNature 的【】外壳
fn trim_corner(s: &str) -> String {
    let t = trim_semi(s);
    t.trim_start_matches('【').trim_end_matches('】').trim().to_string()
}

/// 从 `= (` 之后开始，取出配平的括号内内容
fn match_close_paren(inner: &str) -> String {
    let mut depth = 1i32;
    let mut end = inner.len();
    for (i, c) in inner.char_indices() {
        match c {
            '(' => depth += 1,
            ')' => {
                depth -= 1;
                if depth == 0 {
                    end = i;
                    break;
                }
            }
            _ => {}
        }
    }
    inner[..end].trim().to_string()
}

/// 把 `name(a, b)` 拆成 ("name", "a, b")；括号必须配平（原来用 trim_end_matches(')')
/// 会把 `fact((_n - 1))` 的右括号全吃掉，导致 "undefined function ''"）
fn split_call(expr: &str) -> Option<(String, &str)> {
    let open = expr.find('(')?;
    let callee = expr[..open].trim().to_string();
    let mut depth = 0i32;
    let mut in_str = false;
    for (i, c) in expr.char_indices() {
        if c == '"' {
            in_str = !in_str;
            continue;
        }
        if in_str {
            continue;
        }
        match c {
            '(' => depth += 1,
            ')' => {
                depth -= 1;
                if depth == 0 {
                    return Some((callee, &expr[open + 1..i]));
                }
            }
            _ => {}
        }
    }
    Some((callee, &expr[open + 1..]))
}

fn is_bare_var(s: &str) -> bool {
    let t = s.trim();
    if !t.starts_with('_') || t.len() < 2 {
        return false;
    }
    t.chars().all(|c| c.is_alphanumeric() || c == '_' || c == '🍁')
}

fn nth(args: &[String], i: usize) -> Option<&String> {
    args.get(i).filter(|s| !s.trim().is_empty())
}

fn to_index(v: &Value, len: usize) -> Result<usize, CompileError> {
    let n = v.as_num() as i64;
    let idx = if n < 0 { len as i64 + n } else { n };
    if idx < 0 || idx >= len as i64 {
        return Err(CompileError::new(0, "Runtime",
            &format!("index {} out of range (len {})", n, len)));
    }
    Ok(idx as usize)
}

/// 按最外层逗号切分参数（括号/方括号/全角括号里的逗号不切）
fn split_top_commas(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut depth = 0i32;
    let mut in_str = false;
    let mut cur = String::new();
    for c in s.chars() {
        match c {
            '"' => in_str = !in_str,
            _ if !in_str => match c {
                '(' | '[' | '（' | '［' | '【' | '{' => {
                    depth += 1;
                    cur.push(c);
                }
                ')' | ']' | '）' | '］' | '】' | '}' => {
                    depth -= 1;
                    cur.push(c);
                }
                ',' if depth == 0 => {
                    out.push(cur.trim().to_string());
                    cur.clear();
                }
                _ => cur.push(c),
            },
            _ => cur.push(c),
        }
    }
    if !cur.trim().is_empty() {
        out.push(cur.trim().to_string());
    }
    out
}

fn extract_bracket_content(s: &str) -> &str {
    let s = s.trim();
    if !s.starts_with('[') {
        return s;
    }
    let mut depth = 0i32;
    for (i, c) in s.char_indices() {
        match c {
            '[' => depth += 1,
            ']' => {
                depth -= 1;
                if depth == 0 {
                    return s[1..i].trim();
                }
            }
            _ => {}
        }
    }
    s[1..].trim_end_matches(']').trim()
}

fn find_op(expr: &str, op: &str) -> Option<usize> {
    let mut depth = 0i32;
    let mut in_str = false;
    let mut last = None;
    let chars: Vec<char> = expr.chars().collect();
    let op_chars: Vec<char> = op.chars().collect();
    let op_len = op_chars.len();
    for i in 0..chars.len() {
        match chars[i] {
            '"' => in_str = !in_str,
            _ if !in_str => match chars[i] {
                '(' | '[' | '（' | '［' | '【' => depth += 1,
                ')' | ']' | '）' | '］' | '】' => depth -= 1,
                _ if depth == 0 && i + op_len <= chars.len() => {
                    if chars[i..i + op_len] == op_chars[..] && i > 0 {
                        last = Some(i);
                    }
                }
                _ => {}
            },
            _ => {}
        }
    }
    last
}

fn find_op_char(expr: &str, op: char) -> Option<usize> {
    let mut depth = 0i32;
    let mut in_str = false;
    let mut last = None;
    for (i, c) in expr.char_indices() {
        match c {
            '"' => in_str = !in_str,
            _ if !in_str => match c {
                '(' | '[' | '（' | '［' | '【' => depth += 1,
                ')' | ']' | '）' | '］' | '】' => depth -= 1,
                c if c == op && depth == 0 && i > 0 => last = Some(i),
                _ => {}
            },
            _ => {}
        }
    }
    last
}
