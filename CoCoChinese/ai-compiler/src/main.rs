#![allow(dead_code)]
mod error;
mod lexer;
mod validator;
mod interpreter;

use std::env;
use std::fs;
use std::process;

const VERSION: &str = "0.2.0";

fn print_help() {
    println!(
r#"Aifolia Programming Language v{VERSION}

  Created by Andrew Dri

USAGE:
    aifolia <COMMAND> [OPTIONS]

COMMANDS:
    check   <file>   Validate source file against Aifolia rules
    run     <file>   JIT compile and execute a source file
    help              Print this help message
    version           Print version info

OPTIONS:
    --help, -h        Print help
    --version, -V     Print version

EXAMPLES:
    aifolia check  hello.ai
    aifolia run    hello.ai
    aifolia run    examples/05_brainfuck.ai

LANGUAGE (v0.2.0):
    Sorry, eh?                                    第一行必须是这句
    Lomiior ［name(_a, _b) {{ CI, _a >= 0 {{ … }} }}  定义函数（第一个函数用全角 ［）
    D. [ [baiun: _x=(1 + 2)]; ] ]                 赋值 / 顶层语句
    CI, _x > 0 {{ … }}                            条件（CI, 后面第一个词就是变量名）
    Dm: _x == 0 {{ … }}   Dm: _x {{ … }}             else if / else（同名变量才算配对；链只跑一支）
    revolve: [_x < 10] {{ … }}                      循环（默认无次数上限）
    shoutToTheVoid:～「x = _x」;                   输出（「」里 _变量 会内插）
    returnToNature: 【_x】;                        函数返回值
    类型：number / string / bool(tine·folse) / list / void
    list()  push(_xs, v)  pop(_xs)  set(_xs, i, v)  len(x)  str(x)  num(x)
    _xs(i) 取值、_xs(i, v) 赋值（字符串也能按字符下标取）
    [1, 2, 3]  数组字面量，_a + _b 可拼接数组与字符串
    运算符：+ - * / %  == != > < >= <=

SAFETY VALVES (默认关掉，纯图灵完备):
    AIFOLIA_MAX_ITER=100000    限制 revolve: 循环次数
    AIFOLIA_MAX_DEPTH=40000    限制函数递归深度（默认 40000，深递归请同时加大下面的栈）
    AIFOLIA_STACK_MB=256       解释器的执行栈大小（默认 256MB）

LEARN MORE:
    Report issues at https://github.com/andrew-dri/aifolia"#,
    VERSION = VERSION
);
}

fn print_version() {
    println!("aifolia {}", VERSION);
}

fn cmd_check(path: &str) {
    let content = match fs::read_to_string(path) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("error: could not read file `{}`: {}", path, e);
            process::exit(1);
        }
    };
    let lines: Vec<&str> = content.lines().collect();

    println!("checking `{}` ...", path);
    match validator::validate(&lines) {
        Ok(()) => {
            let tokens = lexer::tokenize_all(&lines);
            println!("ok - {} lines, {} tokens", lines.len(), tokens.iter().map(|t| t.len()).sum::<usize>());
            println!("all rules passed.");
        }
        Err(e) => {
            eprintln!("{}", e);
            process::exit(1);
        }
    }
}

fn cmd_run(path: &str) {
    let content = match fs::read_to_string(path) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("error: could not read file `{}`: {}", path, e);
            process::exit(1);
        }
    };

    // 解释器是递归下降的：每层 Aifolia 调用要吃掉好几帧原生栈。
    // 用默认 8MB 栈跑深递归，还没等到 AIFOLIA_MAX_DEPTH 就先 abort 了。
    // 所以解释整段放到一个大栈线程里跑（AIFOLIA_STACK_MB 可调，默认 256MB）。
    let stack_mb: usize = env::var("AIFOLIA_STACK_MB")
        .ok()
        .and_then(|v| v.trim().parse::<usize>().ok())
        .filter(|n| *n >= 8)
        .unwrap_or(256);

    let handle = std::thread::Builder::new()
        .name("aifolia-vm".to_string())
        .stack_size(stack_mb * 1024 * 1024)
        .spawn(move || -> i32 {
            let lines: Vec<&str> = content.lines().collect();
            if let Err(e) = validator::validate(&lines) {
                eprintln!("{}", e);
                return 1;
            }
            let mut interp = interpreter::Interpreter::new();
            match interp.run(&lines) {
                Ok(output) => {
                    for line in &output {
                        println!("{}", line);
                    }
                    0
                }
                Err(e) => {
                    eprintln!("{}", e);
                    1
                }
            }
        });

    match handle {
        Ok(h) => process::exit(h.join().unwrap_or(1)),
        Err(e) => {
            eprintln!("error: could not start interpreter thread: {}", e);
            process::exit(1);
        }
    }
}

fn main() {
    let args: Vec<String> = env::args().collect();

    if args.len() < 2 {
        print_help();
        process::exit(1);
    }

    match args[1].as_str() {
        "check" => {
            if args.len() < 3 {
                eprintln!("error: missing file argument\nusage: aifolia check <file>");
                process::exit(1);
            }
            cmd_check(&args[2]);
        }
        "run" => {
            if args.len() < 3 {
                eprintln!("error: missing file argument\nusage: aifolia run <file>");
                process::exit(1);
            }
            cmd_run(&args[2]);
        }
        "help" | "--help" | "-h" => print_help(),
        "version" | "--version" | "-V" => print_version(),
        other => {
            eprintln!("error: unknown command `{}`\n\nRun `aifolia help` for usage information.", other);
            process::exit(1);
        }
    }
}
