#ifndef MLK_FAULT_CLIENT_H
#define MLK_FAULT_CLIENT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Unix socket JSON API v1; see docs/声光告警与模型故障API.md.
 * Returns 0 when a complete reply was received; caller must check reply.ok.
 * Returns -1 with errno on transport errors. No Qt dependency. Call from an
 * external process/worker thread, never synchronously on ps-local's UI thread.
 * socket_path=NULL selects /run/mlk-fault.sock; request excludes final newline.
 * timeout_ms covers connect, send and receive together; range 1..10000 ms. */
int mlk_fault_publish_json(const char *socket_path, const char *request,
                          char *reply, size_t reply_size, int timeout_ms);
/* 模型诊断 API 2 的发布入口，请求正文上限 4095 字节；传输、返回码与错误
 * 语义同上方 v1 入口，v1 保留 999 字节上限。 */
int mlk_fault_publish_json_v2(const char *socket_path, const char *request,
                             char *reply, size_t reply_size, int timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
