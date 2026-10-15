// ============================================================
// c_mysql —— 连接（封装 cmysql.c）
//
// Conn 是不透明类型，用户只能经 `var c = connect(...)` 推断持有。
// Conn 为 nocopy + before_destroy，离开作用域、提前 return、try 传播
// 错误时自动关闭。提前关闭用 close(&c)：句柄置 0 保证幂等。
//
// 用到 Conn 的函数必须写在本文件里：struct 不进 import 体系，
// 别的文件写不出 &Conn。
// ============================================================

import { err_msg } from "./cuowu.cp"

extern lib "mariadb"
extern c "cmysql.c"

// ---- cmysql.c ----
extern fn cmysql_connect(host addr(u8), port i64, user addr(u8), pass addr(u8), db addr(u8), timeout_s i64, out addr(i64)) i64
extern fn cmysql_error(h i64) addr(u8)
extern fn cmysql_close(h i64) void

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
