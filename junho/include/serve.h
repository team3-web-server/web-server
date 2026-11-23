#ifndef SERVE_H
#define SERVE_H

#include "http.h"

/* req를 정적 파일 또는 CGI로 처리한다. 0=성공, -1=연결 오류. */
int serve_request(int fd, struct request *req);

#endif
