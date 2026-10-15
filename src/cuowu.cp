// ============================================================
// c_mysql —— 错误码族
//
// e.code 分两类：
//   >= 1000  MySQL 自己的错误号原样透传（服务端 1000+、客户端 2000+）
//   <  1000  本包自己的码：
//     0 未分类   1 ? 个数与参数个数不符   2 结果含非法 UTF-8
//     3 连接已关闭   4 连接选项不合法
//
// err_msg 为内部辅助，门面不导出。
// ============================================================

// err_msg — 统一错误消息："c_mysql.<函数名>: [<码>] <说明>"
export fn err_msg(fname string, code i64, detail string) string {
    return "c_mysql." +[string] fname +[string] ": [" +[string] number_to_string(code) +[string] "] " +[string] detail
}

// ============================================================
// 判定族：收 e.code 返回 bool
// ============================================================

// 唯一键重复
export fn is_duplicate(code i64) bool {
    return code ==[i64] 1062
}

// 用户名或密码错误（1045）、无权访问该库（1044）
export fn is_access_denied(code i64) bool {
    if (code ==[i64] 1044) {
        return true
    }
    return code ==[i64] 1045
}

// 连不上服务端（2002 / 2003）、主机名解析失败（2005）
export fn is_connect_failed(code i64) bool {
    if (code ==[i64] 2002) {
        return true
    }
    if (code ==[i64] 2003) {
        return true
    }
    return code ==[i64] 2005
}

// 连接已断开（2006 服务端已走 / 2013 查询中途断开）
export fn is_disconnected(code i64) bool {
    if (code ==[i64] 2006) {
        return true
    }
    return code ==[i64] 2013
}

// 死锁
export fn is_deadlock(code i64) bool {
    return code ==[i64] 1213
}

// 等锁超时
export fn is_lock_timeout(code i64) bool {
    return code ==[i64] 1205
}
