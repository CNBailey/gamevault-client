#pragma once

#include "http_server.h"

void api_handle(const http_req_t *req, http_res_t *res);
void api_set_port(int port);
int api_port(void);
