TARGET = userspace
OBJS = userspace.o
CFLAGS = -Wall -o2 -g

all: ${TARGET}

${TARGET}: ${OBJS}
	${CC} -o ${TARGET} ${OBJS}

clean:
	rm -f ${TARGET} ${OBJS} core*
