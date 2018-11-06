#include <u.h>
#include <libc.h>
#include <thread.h>

enum {
	Nfileprocs = 4,
	Nblkprocs = 16,

	Blksz = 128*1024,
};

typedef struct {
	Rendez;
	QLock;
	Ref;
} WaitGroup;

typedef struct {
	Dir;
	WaitGroup wg;
	Channel *errchan;
	char *src, *dst;
	int sfd, dfd;
} File;

typedef struct {
	File *f;
	vlong offset;
} Blk;

int errors = 0;
int multisrc = 0;
int keepmode = 0;
int keepmtime = 0;
int keepuser = 0;
int keepgroup = 0;
int blksz = Blksz;
int fileprocs = Nfileprocs;
int blkprocs = Nblkprocs;
Dir *skipdir;

Channel *filechan; /* chan(File*) */
Channel *blkchan; /* chan(Blk*) */

void usage(void);
void *emalloc(ulong);
char *estrdup(char*);

extern int cas(long *p, long ov, long nv);
void wginit(WaitGroup*, long);
void wgadd(WaitGroup*, long);
void wgdone(WaitGroup*);
void wgwait(WaitGroup*);

char *filename(char*);
int mkdir(char*, char*, Dir*, Dir**);
int same(Dir*, Dir*);
void clone(char*, char*);
int cloneattr(int, Dir*);
void clonedir(char*, char*);
int clonefile(File*);
File *filenew(char*, char*, Dir*);
void filefree(File*);
void fileproc(void*);
vlong blklist(File*, Blk**);
void blkproc(void*);

void
usage(void)
{
	fprint(2, "usage: %s [-gux] [-b blocksize] [-p fileprocs:blockprocs] from ... to\n", argv0);
	sysfatal("usage");
}

void
error(char *fmt, ...)
{
	va_list arg;
	char err[ERRMAX];

	errors = 1;
	snprint(err, sizeof err, "%s: %s\n", argv0, fmt);
	va_start(arg, fmt);
	vfprint(2, err, arg);
	va_end(arg);
}

void *
emalloc(ulong n)
{
	void *p;

	p = malloc(n);
	if(p == nil)
		sysfatal("malloc: %r");
	return p;
}

char *
estrdup(char *s)
{
	char *p;

	p = strdup(s);
	if(p == nil)
		sysfatal("strdup: %r");
	return p;
}

void
wginit(WaitGroup *wg, long n)
{
	memset(wg, 0, sizeof(*wg));
	wg->l = &wg->QLock;
	if(cas(&wg->ref, 0, n) == 0)
		sysfatal("wginit: cas failed");
}

void
wgadd(WaitGroup *wg, long n)
{
	long v;

	v = wg->ref;
	while(cas(&wg->ref, v, v+n) == 0)
		v = wg->ref;
}

void
wgdone(WaitGroup *wg)
{
	if(decref(wg) == 0){
		qlock(wg);
		rwakeupall(wg);
		qunlock(wg);
	}
}

void
wgwait(WaitGroup *wg)
{
	qlock(wg);
	while(wg->ref != 0)
		rsleep(wg);
	qunlock(wg);
}

char *
filename(char *s)
{
	char *p;

	p = strrchr(s, '/');
	if(p == nil || p == s)
		return s;
	if(p[1] == 0){
		*p = 0;
		return filename(s);
	}
	return p + 1;
}

int
mkdir(char *src, char *dst, Dir *sd, Dir **dd)
{
	int fd;
	Dir d;
	
	if(!(sd->mode & 0400)){
		error("can't clone directory: '%s' permission denied", src);
		return -1;
	}
	d = *sd;
	d.mode = d.mode | DMDIR | 0200;
	fd = create(dst, 0, d.mode);
	if(fd < 0){
		error("can't create directory: %r");
		return -1;
	}
	if(cloneattr(fd, &d) < 0){
		close(fd);
		return -1;
	}
	if(dd){
		*dd = dirfstat(fd);
		if(*dd == nil){
			error("can't stat: %r");
			close(fd);
			return -1;
		}
	}
	return 1;
}

int
same(Dir *a, Dir *b)
{
	if(a->type == b->type &&
		a->dev == b->dev &&
		a->qid.path == b->qid.path &&
		a->qid.type == b->qid.type &&
		a->qid.vers == b->qid.vers)
		return 1;
	return 0;
}

File *
filenew(char *src, char *dst, Dir *d)
{
	File *f;

	f = emalloc(sizeof(File));
	memmove(f, d, sizeof(Dir));
	f->uid = estrdup(d->uid);
	f->gid = estrdup(d->gid);
	f->src = estrdup(src);
	f->dst = estrdup(dst);
	f->sfd = -1;
	f->dfd = -1;
	f->errchan = chancreate(sizeof(ulong), 0);

	return f;
}

void
filefree(File *f)
{
	if(f->sfd >= 0)
		close(f->sfd);
	if(f->dfd >= 0)
		close(f->dfd);
	free(f->uid);
	free(f->gid);
	free(f->src);
	free(f->dst);
	chanfree(f->errchan);
	free(f);
}

int
cloneattr(int fd, Dir *d)
{
	Dir dd;

	if(!(keepmode || keepuser || keepgroup || keepmtime))
		return 1;
	nulldir(&dd);
	if(keepmode)
		dd.mode = d->mode & DMDIR ? d->mode|0200 : d->mode;
	if(keepmtime)
		dd.mtime = d->mtime;
	if(keepuser)
		dd.uid = d->uid;
	if(keepgroup)
		dd.gid = d->gid;
	if(dirfwstat(fd, &dd) < 0){
		error("can't wstat: %r");
		return -1;
	}
	return 1;
}

void
clone(char *src, char *dst)
{
	Dir *sd, *dd;
	File *f;
	
	dd = nil;
	sd = dirstat(src);
	if(sd == nil){
		error("can't stat: %r");
		return;
	}
	if(access(dst, AEXIST) >= 0){
		dd = dirstat(dst);
		if(dd == nil){
			error("can't stat: %r");
			goto End;
		}
	}else if(multisrc){
		if(mkdir(src, dst, sd, &dd) < 0)
			goto End;
		skipdir = dd;
	}

	/* clone a file */
	if(!(sd->mode & DMDIR)){
		if(dd && dd->mode & DMDIR)
			dst = smprint("%s/%s", dst, filename(src));
		f = filenew(src, dst, sd);
		sendp(filechan, f);
		goto End;
	}

	/* clone a directory */
	if(dd)
		dst = smprint("%s/%s", dst, filename(src));
	if(skipdir){
		if(mkdir(src, dst, sd, nil) < 0)
			goto End;
	}else{
		if(mkdir(src, dst, sd, &skipdir) < 0)
			goto End;
	}
	clonedir(src, dst);

End:
	if(dd) free(dst);
	free(sd); free(dd);
}

void
clonedir(char *src, char *dst)
{
	int fd;
	long n;
	char *sn, *dn;
	Dir *dirs, *d;
	File *f;

	dirs = nil;

	fd = open(src, OREAD);
	if(fd < 0){
		error("can't open: %r");
		return;
	}
	n = dirreadall(fd, &dirs);
	if(n < 0){
		error("can't read directory: %r");
		close(fd);
		return;
	}
	close(fd);

	for(d = dirs; n; n--, d++){
		if(d->mode & DMDIR && same(skipdir, d))
			continue;

		sn = smprint("%s/%s", src, d->name);
		dn = smprint("%s/%s", dst, d->name);
		if(d->mode & DMDIR){
			if(mkdir(sn, dn, d, nil) < 0)
				continue;
			clonedir(sn, dn);
		}else{
			f = filenew(sn, dn, d);
			sendp(filechan, f);
		}
		free(sn);
		free(dn);
	}
	free(dirs);
}

vlong
blklist(File *f, Blk **bp)
{
	vlong i, nblk;
	Blk *b, *p;

	if(f->length == 0)
		return 0;
	nblk = f->length / blksz;
	if(nblk == 0)
		nblk = 1;
	else if(nblk % blksz > 0)
		nblk++;

	b = p = emalloc(sizeof(Blk) * nblk);
	for(i = 0; i < nblk; i++, p++){
		p->f = f;
		p->offset = blksz * i;
	}

	*bp = b;
	return nblk;
}

int
clonefile(File *f)
{
	int ret;
	vlong n;
	Blk *blks, *b, *be;
	enum {Anext, Aerr, Aend};
	Alt alts[] = {
	[Anext] {blkchan, &b, CHANSND},
	[Aerr] {f->errchan, nil, CHANRCV},
	[Aend] {nil, nil, CHANEND},
	};

	ret = 1;
	n = blklist(f, &blks);
	if(n == 0)
		return 1;
	wginit(&f->wg, 0);
	for(b = blks, be = b + n; b != be; b++)
		switch(alt(alts)){
		case Anext:
			wgadd(&f->wg, 1);
			break;
		case Aerr:
			ret = -1;
			goto End;
		}
End:
	chanclose(f->errchan);
	wgwait(&f->wg);
	free(blks);
	return ret;
}

void
blkproc(void *)
{
	int sfd, dfd;
	long n;
	vlong off;
	char *buf;
	File *f;
	Blk *b;

	buf = emalloc(blksz);
	for(;;){
		b = recvp(blkchan);
		if(b == nil)
			break;

		f = b->f;
		sfd = f->sfd;
		dfd = f->dfd;
		off = b->offset;
		if((n = pread(sfd, buf, blksz, off)) < 0){
			error("can't read: %r");
			sendul(f->errchan, ~0);
		}
		if(n > 0 && pwrite(dfd, buf, n, off) < n){
			error("can't write: %r");
			sendul(f->errchan, ~0);
		}

		wgdone(&f->wg);
	}
}

void
fileproc(void *v)
{
	File *f;
	WaitGroup *wg;
	
	wg = v;
	for(;;){
		f = recvp(filechan);
		if(f == nil)
			break;

		f->sfd = open(f->src, OREAD);
		if(f->sfd < 0){
			error("can't open: %r");
			goto End;
		}
		f->dfd = create(f->dst, OWRITE, f->mode);
		if(f->dfd < 0){
			error("can't create: %r");
			goto End;
		}

		if(clonefile(f) > 0)
			cloneattr(f->dfd, f);
End:
		filefree(f);
	}
	wgdone(wg);
}

void
threadmain(int argc, char *argv[])
{
	int i;
	char *dst, *p;
	WaitGroup filewg;

	ARGBEGIN{
	case 'b':
		blksz = strtol(EARGF(usage()), nil, 0);
		break;
	case 'p':
		fileprocs = strtol(EARGF(usage()), &p, 0);
		*p++ = 0;
		blkprocs = strtol(p, nil, 0);
		break;
	case 'x':
		keepmode = keepmtime = 1;
		break;
	case 'u':
		keepuser = 1;
		break;
	case 'g':
		keepgroup = 1;
		break;
	}ARGEND;
	if(argc < 2)
		usage();
	if(argc > 2)
		multisrc = 1;
	dst = argv[argc - 1];
	
	filechan = chancreate(sizeof(File*), fileprocs);
	blkchan = chancreate(sizeof(Blk*), blkprocs);
	wginit(&filewg, fileprocs);
	for(i = 0; i < fileprocs; i++)
		proccreate(fileproc, &filewg, mainstacksize);
	for(i = 0; i < blkprocs; i++)
		proccreate(blkproc, nil, mainstacksize);

	for(i = 0; i < argc -1; i++)
		clone(argv[i], dst);
	chanclose(filechan);
	wgwait(&filewg);

	if(errors)
		threadexitsall("errors");
	threadexitsall(nil);
}
