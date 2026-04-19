PROG=	airpods-ctl
SRCS=	airpods-ctl.c
PREFIX?=	/usr/local
BINDIR=	${PREFIX}/bin

MAN=
LDFLAGS+=	-lbluetooth

CFLAGS+=	-Wall -O2

.include <bsd.prog.mk>
