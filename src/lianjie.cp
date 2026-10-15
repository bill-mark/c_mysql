// ============================================================
// c_mysql —— 连接（封装 cmysql.c）
//
// Conn 是不透明类型，用户只能经 `var c = connect(...)` 推断持有。
// Conn 为 nocopy + before_destroy，离开作用域、提前 return、try 传播
// 错误时自动关闭。提前关闭用 close(&c)：句柄置 0 保证幂等。
//
// 用到 Conn 的函数必须写在本文件里：struct 不进 import 体系，
// 别的文件写不出 &Conn。借用参数也不能再转借，所以内部辅助函数
// 收的是句柄 h 与 at，不收 &Conn。
// ============================================================

import { err_msg } from "./cuowu.cp"

extern lib "mariadb"
extern c "cmysql.c"

// ---- cmysql.c ----
extern fn cmysql_connect(host addr(u8), port i64, user addr(u8), pass addr(u8), db addr(u8), timeout_s i64, out addr(i64)) i64
extern fn cmysql_error(h i64) addr(u8)
extern fn cmysql_close(h i64) void
extern fn cmysql_stmt_new(h i64, sql addr(u8)) i64
extern fn cmysql_stmt_bind(st i64, val addr(u8), is_null i64) i64
extern fn cmysql_stmt_run(st i64) i64
extern fn cmysql_stmt_msg(st i64) addr(u8)
extern fn cmysql_stmt_free(st i64) void
extern fn cmysql_exec_finish(h i64, affected addr(i64), insert_id addr(i64)) i64
extern fn cmysql_store(h i64, out addr(i64)) i64
extern fn cmysql_res_ncols(r i64) i64
extern fn cmysql_res_colname(r i64, i i64) addr(u8)
extern fn cmysql_res_next(r i64) i64
extern fn cmysql_res_cell(r i64, i i64) addr(u8)
extern fn cmysql_res_cell_len(r i64, i i64) i64
extern fn cmysql_res_free(r i64) void

// ============================================================
// Options — 连接选项
//   除 user 外均有默认值，调用方用无名字面量只写需要的字段：
//     connect({ user: "cpro", password: "cpro123", database: "cpro_ceshi" })
// ============================================================
struct Options {
    host       type string : "127.0.0.1",
    port       type i64    : 3306,
    user       type string,
    password   type string : "",
    database   type string : "",
    timeout_ms type i64    : 10000,
}

// ============================================================
// Conn — 一条已建立的连接
//   h  : cmysql 句柄（MYSQL*），0 表示已关闭
//   at : "用户@主机:端口"，用于错误消息
// ============================================================
nocopy struct Conn {
    h  type i64,
    at type string,
}

impl Conn:c {
    // 销毁钩子：未关闭则关闭
    fn before_destroy() void {
        if (c.h !=[i64] 0) {
            unsafe {
                cmysql_close(c.h)
            }
        }
    }
}

// ============================================================
// connect — 建立连接
//   字符集固定 utf8mb4。timeout_ms 只管连接阶段，按秒向上取整，<= 0 不限。
//   选项不合法（端口越界、user 为空）→ 码 4，不发起连接。
//   Conn 须在 return 处就地构造。
// ============================================================
export fn connect(opt Options) Conn fails {
    var at type string = opt.user +[string] "@" +[string] opt.host +[string] ":" +[string] number_to_string(opt.port)

    if (opt.port <[i64] 1) {
        return Err(4, err_msg("connect", 4, "端口越界: " +[string] at))
    }
    if (opt.port >[i64] 65535) {
        return Err(4, err_msg("connect", 4, "端口越界: " +[string] at))
    }
    if (opt.user ==[string] "") {
        return Err(4, err_msg("connect", 4, "user 不能为空: " +[string] at))
    }

    var timeout_s type i64 = 0
    if (opt.timeout_ms >[i64] 0) {
        timeout_s = opt.timeout_ms +[i64] 999
        timeout_s = timeout_s /[i64] 1000
    }

    var h type i64 = 0
    var code type i64 = 0
    var detail type string = ""
    unsafe {
        code = cmysql_connect(c_str(opt.host), opt.port, c_str(opt.user),
                              c_str(opt.password), c_str(opt.database),
                              timeout_s, addr_of(h))
        if (code !=[i64] 0) {
            if (h !=[i64] 0) {
                detail = string_from_c(cmysql_error(h))
                cmysql_close(h)
            } else {
                detail = "内存不足"
            }
        }
    }
    if (code !=[i64] 0) {
        return Err(code, err_msg("connect", code, detail +[string] " (" +[string] at +[string] ")"))
    }

    return { h: h, at: at }
}

// ============================================================
// close — 关闭连接
//   幂等：已关闭时直接返回 true。cpro 不支持 void fails，成功恒为 true。
// ============================================================
export fn close(c &Conn) bool fails {
    if (c.h ==[i64] 0) {
        return true
    }
    unsafe {
        cmysql_close(c.h)
    }
    c.h = 0
    return true
}

// ============================================================
// ExecResult — exec 的结果
//   affected_rows : 影响的行数（语句带结果集时为结果行数）
//   insert_id     : 最后一次插入生成的自增 id，没有生成时为 0
// ============================================================
struct ExecResult {
    affected_rows type i64,
    insert_id     type i64,
}

// ============================================================
// Rows — query 的结果
//   columns : 列名，按 SELECT 的顺序
//   rows    : 每行一个数组，下标与 columns 对应；SQL 的 NULL 是 none
// ============================================================
struct Rows {
    columns type [string],
    rows    type [[optional(string)]],
}

// ============================================================
// run_sql — 内部：展开 ? 占位符并执行一条语句
//   结果留在连接上，由调用方接着取（exec_finish / store）。
//   ? 的展开与转义在 cmysql.c 里做：引号与注释里的 ? 不算，
//   个数不符报码 1 且语句不发出。
// ============================================================
fn run_sql(fname string, h i64, at string, sql string, params [optional(string)]) bool fails {
    if (h ==[i64] 0) {
        return Err(3, err_msg(fname, 3, "连接已关闭 (" +[string] at +[string] ")"))
    }

    var st type i64 = 0
    unsafe {
        st = cmysql_stmt_new(h, c_str(sql))
    }
    if (st ==[i64] 0) {
        return Err(2008, err_msg(fname, 2008, "内存不足 (" +[string] at +[string] ")"))
    }

    var code type i64 = 0
    for (p in params) {
        if (code ==[i64] 0) {
            if (p == none) {
                unsafe {
                    code = cmysql_stmt_bind(st, raw_to_addr(0, u8), 1)
                }
            } else {
                var v type string = unwrap(p, "")
                unsafe {
                    code = cmysql_stmt_bind(st, c_str(v), 0)
                }
            }
        }
    }

    var detail type string = "内存不足"
    unsafe {
        if (code ==[i64] 0) {
            code = cmysql_stmt_run(st)
            if (code ==[i64] 1) {
                detail = string_from_c(cmysql_stmt_msg(st))
            }
            if (code >[i64] 1) {
                detail = string_from_c(cmysql_error(h))
            }
        }
        cmysql_stmt_free(st)
    }

    if (code !=[i64] 0) {
        return Err(code, err_msg(fname, code, detail +[string] " (" +[string] at +[string] ")"))
    }
    return true
}

// ============================================================
// exec — 执行不返回结果集的语句（INSERT / UPDATE / DELETE / DDL）
//   语句若带结果集（误用在 SELECT 上），结果读完丢弃，affected_rows 为行数。
// ============================================================
export fn exec(c &Conn, sql string, params [optional(string)] = []) ExecResult fails {
    var ok type bool = up_fails run_sql("exec", c.h, c.at, sql, params)

    var aff type i64 = 0
    var iid type i64 = 0
    var code type i64 = 0
    var detail type string = ""
    unsafe {
        code = cmysql_exec_finish(c.h, addr_of(aff), addr_of(iid))
        if (code !=[i64] 0) {
            detail = string_from_c(cmysql_error(c.h))
        }
    }
    if (code !=[i64] 0) {
        return Err(code, err_msg("exec", code, detail +[string] " (" +[string] c.at +[string] ")"))
    }

    return { affected_rows: aff, insert_id: iid }
}

// ============================================================
// query — 执行查询，结果整个读进内存
//   所有值以文本返回（MySQL 客户端里看到的样子），NULL 为 none。
//   语句没有结果集（如 UPDATE）时返回空的 columns 与 rows。
//   某个值不是合法 UTF-8（多为二进制列）→ 码 2，连接仍可继续使用。
// ============================================================
export fn query(c &Conn, sql string, params [optional(string)] = []) Rows fails {
    var ok type bool = up_fails run_sql("query", c.h, c.at, sql, params)

    var cols type [string] = []
    var rows type [[optional(string)]] = []

    var rh type i64 = 0
    var code type i64 = 0
    var detail type string = ""
    unsafe {
        code = cmysql_store(c.h, addr_of(rh))
        if (code !=[i64] 0) {
            detail = string_from_c(cmysql_error(c.h))
        }
    }
    if (code !=[i64] 0) {
        return Err(code, err_msg("query", code, detail +[string] " (" +[string] c.at +[string] ")"))
    }
    if (rh ==[i64] 0) {
        return { columns: cols, rows: rows }
    }

    var n type i64 = 0
    unsafe {
        n = cmysql_res_ncols(rh)
    }
    var i type i64 = 0
    while (i <[i64] n) {
        unsafe {
            cols.push(string_from_c(cmysql_res_colname(rh, i)))
        }
        i = i +[i64] 1
    }

    // 出错时不能在循环里直接 return：rh 必须先释放
    var bad type bool = false
    var bad_row type i64 = 0
    var bad_col type i64 = 0
    while (true) {
        var got type i64 = 0
        unsafe {
            got = cmysql_res_next(rh)
        }
        if (got ==[i64] 0) {
            break
        }

        var row type [optional(string)] = []
        var j type i64 = 0
        while (j <[i64] n) {
            var is_null type bool = false
            var bytes type [u8] = []
            unsafe {
                var p type addr(u8) = cmysql_res_cell(rh, j)
                if (addr_to_raw(p) ==[i64] 0) {
                    is_null = true
                } else {
                    var len type i64 = cmysql_res_cell_len(rh, j)
                    if (len >[i64] 0) {
                        bytes = array_from_addr(p, len, u8)
                    }
                }
            }
            if (is_null) {
                var v type optional(string) = none
                row.push(v)
            } else {
                try {
                    var s type string = string.from_utf8(bytes)
                    var v type optional(string) = s
                    row.push(v)
                } catch (e) {
                    bad = true
                    bad_row = rows.length() +[i64] 1
                    bad_col = j
                }
            }
            if (bad) {
                break
            }
            j = j +[i64] 1
        }
        if (bad) {
            break
        }
        rows.push(row)
    }

    unsafe {
        cmysql_res_free(rh)
    }

    if (bad) {
        var weizhi type string = "第 " +[string] number_to_string(bad_row) +[string] " 行第 " +[string] number_to_string(bad_col +[i64] 1) +[string] " 列"
        weizhi = weizhi +[string] "（" +[string] cols[bad_col] +[string] "）不是合法 UTF-8，第一版不支持读二进制数据"
        return Err(2, err_msg("query", 2, weizhi +[string] " (" +[string] c.at +[string] ")"))
    }

    return { columns: cols, rows: rows }
}

// ============================================================
// column_index — 按列名找列下标，找不到返回 -1
//   逐字比较、区分大小写；重名列返回第一个。
// ============================================================
export fn column_index(r &Rows, name string) i64 {
    var i type i64 = 0
    var n type i64 = r.columns.length()
    while (i <[i64] n) {
        if (r.columns[i] ==[string] name) {
            return i
        }
        i = i +[i64] 1
    }
    return 0 -[i64] 1
}
