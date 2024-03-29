#include <u.h>
#include <libc.h>
#include <thread.h>

enum {
	Nfileprocs = 4,
	Nblkprocs = 16,

	Blksz = 128*1024,
};

typedef struct Waitgroup Waitgroup;
typedef struct File File;
typedef struct Blk Blk;

struct Waitgroup {
	Rendez;
	QLock;
	Ref;
};

struct File {
	Dir;
	Waitgroup wg;
	Channel *errchan;
	char *src, *dst;
	int sfd, dfd;
};

struct Blk {
	File *f;
	long sz;
	vlong offset;
};

int errors = 0;
int multisrc = 0;
int archive = 0;
int notemp = 0;
int blksz = Blksz;
int fileprocs = Nfileprocs;
int blkprocs = Nblkprocs;
long salt;
Dir *skipdir;

Channel *filechan; /* chan(File*) */
Channel *blkchan; /* chan(Blk*) */

void
usage(void)
{
	fprint(2, "usage: %s [-aT] [-b blocksize] [-p fileprocs:blockprocs] from ... to\n", argv0);
	exits("usage");
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

extern int cas(long *p, long ov, long nv);

void
wginit(Waitgroup *wg, long n)
{
	memset(wg, 0, sizeof(*wg));
	wg->l = &wg->QLock;
	if(cas(&wg->ref, 0, n) == 0)
		sysfatal("wginit: cas failed");
}

void
wgadd(Waitgroup *wg, long n)
{
	long v;

	v = wg->ref;
	while(cas(&wg->ref, v, v+n) == 0)
		v = wg->ref;
}

void
wgdone(Waitgroup *wg)
{
	if(decref(wg) == 0){
		qlock(wg);
		rwakeupall(wg);
		qunlock(wg);
	}
}

void
wgwait(Waitgroup *wg)
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

	if(!(archive))
		return 1;
	nulldir(&dd);
	dd.mode = d->mode&DMDIR ? d->mode|0200 : d->mode;
	dd.mtime = d->mtime;
	dd.gid = d->gid;
	if(dirfwstat(fd, &dd) < 0){
		error("can't wstat: %r");
		return -1;
	}
	return 1;
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
	close(fd);
	return 1;
}

void
dmtimeperms(char *src, char *dst)
{
	int fd;
	long n;
	char *sn, *dn;
	Dir *dirs, *d, dd, *s;

	dirs = nil;

	fd = open(src, OREAD);
	if(fd < 0){
		error("can't open: %r");
		return;
	}
	while((n = dirread(fd, &dirs)) > 0){
		for(d = dirs; n; n--, d++){
			sn = smprint("%s/%s", src, d->name);
			dn = smprint("%s/%s", dst, d->name);
			if(d->mode & DMDIR)
				dmtimeperms(sn, dn);
			free(sn);
			free(dn);
		}
		free(dirs);
	}
	if(n < 0)
		error("can't read directory: %r");
	close(fd);

	d = dirstat(src);
 	if(archive && d->mode&DMDIR){
		s = dirstat(dst);
		if(s->mode&DMDIR){
			nulldir(&dd);
			dd.mode = d->mode;
			dd.mtime = d->mtime;
			if(dirwstat(dst, &dd) < 0)
				error("can't dirwstat: %r");
		}
	}
	free(d);
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
	while((n = dirread(fd, &dirs)) > 0){
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
	if(n < 0)
		error("can't read directory: %r");
	close(fd);
}

void
clone(char *src, char *dst)
{
	char *dn;
	Dir *sd, *dd;
	File *f;
	
	dn = estrdup(dst);
	dd = nil;
	sd = dirstat(src);
	if(sd == nil){
		error("can't stat: %r");
		return;
	}
	if(access(dn, AEXIST) >= 0){
		dd = dirstat(dn);
		if(dd == nil){
			error("can't stat: %r");
			goto End;
		}
	}else if(multisrc){
		if(mkdir(src, dn, sd, &dd) < 0)
			goto End;
		skipdir = dd;
	}

	/* clone a file */
	if(!(sd->mode & DMDIR)){
		if(dd && dd->mode & DMDIR)
				dn = smprint("%s/%s", dn, filename(src));
		f = filenew(src, dn, sd);
		sendp(filechan, f);
		goto End;
	}

	/* clone a directory */
	if(dd)
		dn = smprint("%s/%s", dn, filename(src));
	if(skipdir){
		if(mkdir(src, dn, sd, nil) < 0)
			goto End;
	}else{
		if(mkdir(src, dn, sd, &skipdir) < 0)
			goto End;
	}
	clonedir(src, dn);

End:
	free(dn);
	free(sd);
	free(dd);
}

vlong
blklist(File *f, Blk **bp)
{
	long odd;
	vlong i, nblk;
	Blk *b, *p;

	if(f->length == 0)
		return 0;
	odd = f->length % blksz;
	nblk = f->length / blksz + (odd > 0);
	b = p = emalloc(sizeof(Blk) * nblk);
	for(i = 0; i < nblk; i++, p++){
		p->f = f;
		p->sz = blksz;
		p->offset = blksz * i;
	}
	if(odd > 0)
		b[nblk-1].sz = odd;

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

long
preadn(int fd, void *buf, long nbytes, vlong offset)
{
	long nr, n;
	vlong o;
	char *p;
	
	nr = 0, n = nbytes, o = offset, p = buf;
	while(nr < nbytes){
		if((n = pread(fd, p, n, o)) < 0)
			return -1;
		if(n == 0)
			break;
		nr += n, o += n, p += n;
	}
	return nr;
}

void
blkproc(void *)
{
	int sfd, dfd;
	long n;
	char *buf;
	File *f;
	Blk *b;
	
	threadsetname("blkproc");

	buf = emalloc(blksz);
	for(;;){
		b = recvp(blkchan);
		if(b == nil)
			break;

		f = b->f;
		sfd = f->sfd;
		dfd = f->dfd;
		if((n = preadn(sfd, buf, b->sz, b->offset)) < 0){
			error("can't read: %r");
			sendul(f->errchan, ~0);
		}
		if(n > 0 && pwrite(dfd, buf, n, b->offset) != n){
			error("can't write: %r");
			sendul(f->errchan, ~0);
		}

		wgdone(&f->wg);
	}
}

void
fileproc(void *v)
{
	char *dst;
	Dir d;
	File *f;
	Waitgroup *wg;
	
	threadsetname("fileproc");
	
	wg = v;
	for(;;){
		f = recvp(filechan);
		if(f == nil)
			break;

		dst = nil;
		f->sfd = open(f->src, OREAD);
		if(f->sfd < 0){
			error("can't open: %r");
			goto End;
		}
		if(notemp)
			dst = estrdup(f->dst);
		else
			dst = smprint("%s.clone.%ld", f->dst, salt);
		f->dfd = create(dst, OWRITE, f->mode);
		if(f->dfd < 0){
			error("can't create: %r");
			goto End;
		}
		if(clonefile(f) < 0){
			if(remove(dst) < 0)
				error("can't remove: %r");
			goto End;
		}
		cloneattr(f->dfd, f);
		if(notemp)
			goto End;
		if(dirstat(f->dst) != nil && remove(f->dst) < 0){
			error("can't remove: %r");
			goto End;
		}
		nulldir(&d);
		d.name = filename(f->dst);
		if(dirfwstat(f->dfd, &d) < 0){
			error("dirfwstat: %r");
			goto End;
		}
		
End:
		filefree(f);
		free(dst);
	}
	wgdone(wg);
}

void
threadmain(int argc, char *argv[])
{
	int i;
	char *dst, *p;
	Waitgroup filewg;

	salt = time(0);
	ARGBEGIN{
	case 'b':
		blksz = strtol(EARGF(usage()), nil, 0);
		break;
	case 'p':
		fileprocs = strtol(EARGF(usage()), &p, 0);
		*p++ = 0;
		blkprocs = strtol(p, nil, 0);
		break;
	case 'a':
		archive = 1;
		break;
	case 'T':
		notemp = 1;
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
	if(!errors)
		/* Update all destination directory mtimes */
		for(i = 0; i < argc -1; i++)
			if(access(dst, AEXIST) >= 0)
				dmtimeperms(argv[i], dst);
	if(errors)
		threadexitsall("errors");
	threadexitsall(nil);
}
