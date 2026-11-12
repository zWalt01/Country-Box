// Country-Box: the services of ipc_services.c, listed in ipc.c's sm: table.
#pragma once
#include "ipc.h"

void ipc_service_lm(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);
void ipc_service_audout(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);   // audout.c
void ipc_service_set(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);   // set (ISettingsServer)
