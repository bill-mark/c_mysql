/* ============================================================
 * c_mysql 的 C 薄包装（由 lianjie.cp 的 extern c 带进链接）
 *
 * cpro 侧只拿 int64_t 句柄和标量，MYSQL / MYSQL_RES 的布局只在这里出现。
 * 字符串入参一律是 cpro 的 c_str（以 NUL 结尾、不含内部 NUL）。
 *
 * 返回码：0 成功；其余为 MySQL 错误号（客户端 2000+、服务端 1000+），
 * 或 c_mysql 自己的码（< 1000，见 cuowu.cp）。
 * ============================================================ */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mariadb/mysql.h>

#define CM_PLACEHOLDER 1     /* ? 个数与参数个数不符 */
#define CM_NOMEM       2008  /* CR_OUT_OF_MEMORY */
#define CM_UNKNOWN     2000  /* CR_UNKNOWN_ERROR */

#define H2M(h) ((MYSQL *)(intptr_t)(h))

/* strdup 在 -std=c11 下不可见，自带一份 */
static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ---------------- 连接 ---------------- */

/* 失败时 *out 仍可能是有效句柄（用来取错误信息），调用方取完必须 cmysql_close */
int64_t cmysql_connect(const char *host, int64_t port, const char *user,
                       const char *pass, const char *db, int64_t timeout_s,
                       int64_t *out) {
    *out = 0;
    MYSQL *m = mysql_init(NULL);
    if (!m) return CM_NOMEM;
    *out = (int64_t)(intptr_t)m;

    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (timeout_s > 0) {
        unsigned int t = (unsigned int)timeout_s;
        mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &t);
    }
    /* 关掉 LOAD DATA LOCAL：服务端可借它读客户端本机任意文件 */
    unsigned int no_local = 0;
    mysql_options(m, MYSQL_OPT_LOCAL_INFILE, &no_local);

    const char *dbname = (db && db[0]) ? db : NULL;
    /* 不带 CLIENT_MULTI_STATEMENTS：一次只许一条语句 */
    if (!mysql_real_connect(m, host, user, pass, dbname, (unsigned int)port, NULL, 0)) {
        unsigned int e = mysql_errno(m);
        return e ? (int64_t)e : CM_UNKNOWN;
    }
    return 0;
}

int64_t cmysql_errno(int64_t h) {
    return h ? (int64_t)mysql_errno(H2M(h)) : 0;
}

const char *cmysql_error(int64_t h) {
    return h ? mysql_error(H2M(h)) : "";
}

void cmysql_close(int64_t h) {
    if (h) mysql_close(H2M(h));
}

/* ---------------- 语句：? 占位符展开 ---------------- */

typedef struct {
    MYSQL  *m;
    char   *sql;
    size_t  n, cap;
    char  **vals;        /* NULL 表示 SQL 的 NULL */
    char    msg[160];
} Stmt;

int64_t cmysql_stmt_new(int64_t h, const char *sql) {
    Stmt *s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->m = H2M(h);
    s->sql = dup_str(sql);
    if (!s->sql) { free(s); return 0; }
    return (int64_t)(intptr_t)s;
}

/* is_null 非 0 时忽略 val */
int64_t cmysql_stmt_bind(int64_t st, const char *val, int64_t is_null) {
    Stmt *s = (Stmt *)(intptr_t)st;
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 8;
        char **nv = realloc(s->vals, nc * sizeof *nv);
        if (!nv) return CM_NOMEM;
        s->vals = nv;
        s->cap = nc;
    }
    char *v = NULL;
    if (!is_null) {
        v = dup_str(val);
        if (!v) return CM_NOMEM;
    }
    s->vals[s->n++] = v;
    return 0;
}

void cmysql_stmt_free(int64_t st) {
    Stmt *s = (Stmt *)(intptr_t)st;
    if (!s) return;
    for (size_t i = 0; i < s->n; i++) free(s->vals[i]);
    free(s->vals);
    free(s->sql);
    free(s);
}

/* cmysql_stmt_run 返回 CM_PLACEHOLDER / CM_NOMEM 时的说明文字 */
const char *cmysql_stmt_msg(int64_t st) {
    return ((Stmt *)(intptr_t)st)->msg;
}

/* 按 MySQL 词法跳过引号与注释，返回 p 之后下一个要看的位置；
 * 普通字符返回 p 本身（由调用方处理）。bs = 反斜杠是否为转义符 */
static const char *skip_quoted(const char *p, int bs) {
    char q = *p;
    if (q == '\'' || q == '"' || q == '`') {
        p++;
        while (*p) {
            if (bs && *p == '\\' && q != '`' && p[1]) { p += 2; continue; }
            if (*p == q) {
                if (p[1] == q) { p += 2; continue; }   /* '' 或 `` 表示自身 */
                return p + 1;
            }
            p++;
        }
        return p;
    }
    if (q == '#' || (q == '-' && p[1] == '-' &&
                     (p[2] == '\0' || p[2] == ' ' || p[2] == '\t' ||
                      p[2] == '\n' || p[2] == '\r'))) {
        while (*p && *p != '\n') p++;
        return p;
    }
    if (q == '/' && p[1] == '*') {
        const char *e = strstr(p + 2, "*/");
        return e ? e + 2 : p + strlen(p);
    }
    return p;
}

static size_t count_placeholders(const char *sql, int bs) {
    size_t n = 0;
    const char *p = sql;
    while (*p) {
        const char *q = skip_quoted(p, bs);
        if (q != p) { p = q; continue; }
        if (*p == '?') n++;
        p++;
    }
    return n;
}

/* 展开占位符并执行。之后由 cmysql_exec_finish 或 cmysql_store 取结果 */
int64_t cmysql_stmt_run(int64_t st) {
    Stmt *s = (Stmt *)(intptr_t)st;
    MYSQL *m = s->m;
    int bs = !(m->server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES);

    size_t want = count_placeholders(s->sql, bs);
    if (want != s->n) {
        snprintf(s->msg, sizeof s->msg, "SQL 里有 %zu 个 ?，参数却有 %zu 个", want, s->n);
        return CM_PLACEHOLDER;
    }

    /* 先算出上限：每个值转义后最多 2 倍长，再加两侧引号 */
    size_t cap = strlen(s->sql) + 1;
    for (size_t i = 0; i < s->n; i++)
        cap += s->vals[i] ? strlen(s->vals[i]) * 2 + 2 : 4;
    char *out = malloc(cap);
    if (!out) {
        snprintf(s->msg, sizeof s->msg, "内存不足");
        return CM_NOMEM;
    }

    size_t k = 0, idx = 0;
    const char *p = s->sql;
    while (*p) {
        const char *q = skip_quoted(p, bs);
        if (q != p) {
            memcpy(out + k, p, (size_t)(q - p));
            k += (size_t)(q - p);
            p = q;
            continue;
        }
        if (*p == '?') {
            const char *v = s->vals[idx++];
            if (!v) {
                memcpy(out + k, "NULL", 4);
                k += 4;
            } else {
                out[k++] = '\'';
                k += mysql_real_escape_string(m, out + k, v, (unsigned long)strlen(v));
                out[k++] = '\'';
            }
            p++;
            continue;
        }
        out[k++] = *p++;
    }
    out[k] = '\0';

    int rc = mysql_real_query(m, out, (unsigned long)k);
    free(out);
    if (rc != 0) {
        unsigned int e = mysql_errno(m);
        return e ? (int64_t)e : CM_UNKNOWN;
    }
    return 0;
}

/* 丢掉后续结果集（存储过程会多返回一个），否则下一条语句报 2014 */
static void drain_more(MYSQL *m) {
    while (mysql_next_result(m) == 0) {
        MYSQL_RES *r = mysql_store_result(m);
        if (r) mysql_free_result(r);
    }
}

/* exec 用：取影响行数与自增 id；语句若带结果集则读完丢弃 */
int64_t cmysql_exec_finish(int64_t h, int64_t *affected, int64_t *insert_id) {
    MYSQL *m = H2M(h);
    *affected = 0;
    *insert_id = 0;
    if (mysql_field_count(m) > 0) {
        MYSQL_RES *r = mysql_store_result(m);
        if (!r) {
            unsigned int e = mysql_errno(m);
            return e ? (int64_t)e : CM_UNKNOWN;
        }
        *affected = (int64_t)mysql_num_rows(r);
        mysql_free_result(r);
    } else {
        my_ulonglong a = mysql_affected_rows(m);
        *affected = (a == (my_ulonglong)-1) ? 0 : (int64_t)a;
        *insert_id = (int64_t)mysql_insert_id(m);
    }
    drain_more(m);
    return 0;
}

/* ---------------- 结果集 ---------------- */

typedef struct {
    MYSQL_RES     *res;
    MYSQL_FIELD   *fields;
    MYSQL_ROW      row;
    unsigned long *lens;
    unsigned int   ncols;
} Res;

/* query 用：语句没有结果集（如 UPDATE）时 *out = 0 且返回 0 */
int64_t cmysql_store(int64_t h, int64_t *out) {
    MYSQL *m = H2M(h);
    *out = 0;
    if (mysql_field_count(m) == 0) {
        drain_more(m);
        return 0;
    }
    MYSQL_RES *r = mysql_store_result(m);
    if (!r) {
        unsigned int e = mysql_errno(m);
        return e ? (int64_t)e : CM_UNKNOWN;
    }
    Res *x = calloc(1, sizeof *x);
    if (!x) {
        mysql_free_result(r);
        drain_more(m);
        return CM_NOMEM;
    }
    x->res = r;
    x->fields = mysql_fetch_fields(r);
    x->ncols = mysql_num_fields(r);
    drain_more(m);
    *out = (int64_t)(intptr_t)x;
    return 0;
}

int64_t cmysql_res_ncols(int64_t rh) {
    return (int64_t)((Res *)(intptr_t)rh)->ncols;
}

const char *cmysql_res_colname(int64_t rh, int64_t i) {
    return ((Res *)(intptr_t)rh)->fields[i].name;
}

/* 1 = 取到一行，0 = 没有了 */
int64_t cmysql_res_next(int64_t rh) {
    Res *x = (Res *)(intptr_t)rh;
    x->row = mysql_fetch_row(x->res);
    if (!x->row) return 0;
    x->lens = mysql_fetch_lengths(x->res);
    return 1;
}

/* SQL 的 NULL 返回空指针 */
const char *cmysql_res_cell(int64_t rh, int64_t i) {
    return ((Res *)(intptr_t)rh)->row[i];
}

int64_t cmysql_res_cell_len(int64_t rh, int64_t i) {
    return (int64_t)((Res *)(intptr_t)rh)->lens[i];
}

void cmysql_res_free(int64_t rh) {
    Res *x = (Res *)(intptr_t)rh;
    if (!x) return;
    mysql_free_result(x->res);
    free(x);
}
