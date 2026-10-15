// ============================================================
// c_mysql —— 门面
//
// 第三方包只能具名导入：
//   import { connect, close } from "bill-mark/c_mysql"
// 英文名与拼音别名是同一符号，任选其一。
//
// 实现分布：
//   cuowu.cp    错误消息与判定函数
//   lianjie.cp  Options / Conn / ExecResult / Rows、连接、执行、查询
//   cmysql.c    C 薄包装（libmariadb）
// ============================================================

// ===== 连接与执行（column_index 以外均为 fails）=====
export import { connect,      connect      as lianjie,
                exec,         exec         as zhixing,
                query,        query        as chaxun,
                column_index, column_index as lie_xuhao,
                close,        close        as guanbi } from "./lianjie.cp"

// ===== 错误判定族：收 e.code 返回 bool =====
export import { is_duplicate,      is_duplicate      as shi_chongfu,
                is_access_denied,  is_access_denied  as shi_jujue_fangwen,
                is_connect_failed, is_connect_failed as shi_lianjie_shibai,
                is_disconnected,   is_disconnected   as shi_duankai,
                is_deadlock,       is_deadlock       as shi_sisuo,
                is_lock_timeout,   is_lock_timeout   as shi_suo_chaoshi } from "./cuowu.cp"
