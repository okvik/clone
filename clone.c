#include <u.h>
#include <libc.h>
#include <thread.h>

enum {
	Nfileprocs = 4,
	Nblkprocs = 16,

	Blksz = 128*1024,
	Blkdone = 1,
	
	End = 1,
};

typedef struct {
	Dir;
	char *src, *dst;
	int sfd, dfd;
	Channel *c;
} File;

typedef struct {
	File *f;
	vlong offset;
} Blk;

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
Channel *endchan; /* chan(ulong) */

void usage(void);
void *emalloc(ulong);
char *estrdup(char*);

char *filename(char*);
Dir *mkdir(char*, Dir*, int);
int same(Dir*, Dir*);
void clone(char*, char*);
void cloneattr(int, Dir*);
void clonedir(char*, char*);
void clonefile(File*);
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

void *
emalloc(ulong n)
{
	void *p;

	p = malloc(n);
	if(p == nil)
		sysfatal("malloc");
	return p;
}

char *
estrdup(char *s)
{
	char *p;

	p = strdup(s);
	if(p == nil)
		sysfatal("strdup");
	return p;
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

Dir *
mkdir(char *name, Dir *d, int dostat)
{
	int fd;
	Dir *dd;

	dd = nil;
	fd = create(name, 0, d->mode | 0200);
	if(fd < 0)
		sysfatal("can't create destination directory");
	cloneattr(fd, d);
	if(dostat){
		dd = dirfstat(fd);
		if(dd == nil)
			sysfatal("can't stat");
	}
	close(fd);
	return dd;
}

void
cloneattr(int fd, Dir *d)
{
	Dir dd;

	if(!(keepmode || keepuser || keepgroup || keepmtime))
		return;
	nulldir(&dd);
	if(keepmode)
		dd.mode = d->mode & DMDIR ? d->mode|0200 : d->mode;
	if(keepmtime)
		dd.mtime = d->mtime;
	if(keepuser)
		dd.uid = d->uid;
	if(keepgroup)
		dd.gid = d->gid;
	if(dirfwstat(fd, &dd) < 0)
		sysfatal("can't wstat");
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
	f->c = nil;

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
	free(f);
}

void
clone(char *src, char *dst)
{
	Dir *sd, *dd;
	File *f;
	
	sd = dirstat(src);
	if(sd == nil)
		sysfatal("can't stat");
	dd = nil;
	if(access(dst, AEXIST) >= 0){
		dd = dirstat(dst);
		if(dd == nil)
			sysfatal("can't stat");
	}
	
	/* clone a file */
	if(!(sd->mode & DMDIR)){
		if(dd && dd->mode & DMDIR)
			dst = smprint("%s/%s", dst, filename(src));
		f = filenew(src, dst, sd);
		sendp(filechan, f);
		return;
	}

	/* clone a directory */
	if(dd)
		dst = smprint("%s/%s", dst, filename(src));
	skipdir = mkdir(dst, sd, 1);
	clonedir(src, dst);
}

void
clonedir(char *src, char *dst)
{
	int fd;
	long n;
	char *sn, *dn;
	Dir *dirs, *d;
	File *f;

	fd = open(src, OREAD);
	if(fd < 0)
		sysfatal("can't open");
	n = dirreadall(fd, &dirs);
	if(n < 0)
		sysfatal("can't read directory");
	close(fd);

	for(d = dirs; n; n--, d++){
		if(d->mode & DMDIR && same(skipdir, d))
			continue;

		sn = smprint("%s/%s", src, d->name);
		dn = smprint("%s/%s", dst, d->name);
		if(d->mode & DMDIR){
			mkdir(dn, d, 0);
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

void
clonefile(File *f)
{
	vlong n, done;
	Blk *blks, *b, *be;

	enum {Anext, Adone, Aend};
	Alt alts[] = {
	[Anext] {blkchan, &b, CHANSND},
	[Adone] {f->c, nil, CHANRCV},
	[Aend] {nil, nil, CHANEND},
	};

	n = blklist(f, &blks);
	if(n == 0)
		return;
	b = blks;
	be = blks + n;
	done = 0;
	while(done < n){
		switch(alt(alts)){
		case Anext:
			++b;
			if(b == be)
				alts[Anext].op = CHANNOP;
			break;
		case Adone:
			++done;
			break;
		}
	}
	free(blks);
}

void
blkproc(void *)
{
	int sfd, dfd;
	long n;
	vlong off;
	char *buf;
	Blk *b;

	buf = emalloc(blksz);
	for(;;){
		b = recvp(blkchan);
		if(b == nil)
			break;

		sfd = b->f->sfd;
		dfd = b->f->dfd;
		off = b->offset;
		if((n = pread(sfd, buf, blksz, off)) < 0)
			sysfatal("blkproc: read error");
		if(n > 0)
			if(pwrite(dfd, buf, n, off) < n)
				sysfatal("blkproc: write error");

		sendul(b->f->c, Blkdone);
	}
}

void
fileproc(void *)
{
	Channel *c;
	File *f;

	c = chancreate(sizeof(ulong), blkprocs);
	for(;;){
		f = recvp(filechan);
		if(f == nil){
			sendul(endchan, End);
			return;
		}

		f->c = c;
		f->sfd = open(f->src, OREAD);
		if(f->sfd < 0)
			sysfatal("fileproc: can't open");
		f->dfd = create(f->dst, OWRITE, f->mode);
		if(f->dfd < 0)
			sysfatal("fileproc: can't create");

		clonefile(f);
		cloneattr(f->dfd, f);
		filefree(f);
	}
}

void
threadmain(int argc, char *argv[])
{
	int i;
	char *dst, *p;

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
	dst = argv[argc - 1];

	filechan = chancreate(sizeof(File*), fileprocs);
	blkchan = chancreate(sizeof(Blk*), blkprocs);
	endchan = chancreate(sizeof(ulong), 0);
	for(i = 0; i < fileprocs; i++)
		proccreate(fileproc, nil, mainstacksize);
	for(i = 0; i < blkprocs; i++)
		proccreate(blkproc, nil, mainstacksize);

	for(i = 0; i < argc -1; i++)
		clone(argv[i], dst);

	for(i = 0; i < fileprocs; i++){
		sendp(filechan, nil);
		recvul(endchan);
	}

	threadexitsall(nil);
}
