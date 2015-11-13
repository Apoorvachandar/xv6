//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "fcntl.h"
#include "pipe.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=proc->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;

  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd] == 0){
      proc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

int
sys_dup(void)
{
  struct file *f;
  int fd;
  
  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

int
sys_read(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return fileread(f, p, n);
}

int
sys_write(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return filewrite(f, p, n);
}

int
sys_close(void)
{
  int fd;
  struct file *f;
  
  if(argfd(0, &fd, &f) < 0)
    return -1;
  proc->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

int
sys_fstat(void)
{
  struct file *f;
  struct stat *st;
  
  if(argfd(0, 0, &f) < 0 || argptr(1, (void*)&st, sizeof(*st)) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
int
sys_link(void)
{
  char name[DIRSIZ], *new, *old;
  struct inode *dp, *ip;

  if(argstr(0, &old) < 0 || argstr(1, &new) < 0)
    return -1;
  if((ip = namei(old)) == 0)
    return -1;

  begin_trans();

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    commit_trans();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  commit_trans();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  commit_trans();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

//PAGEBREAK!
int
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], *path;
  uint off;

  if(argstr(0, &path) < 0)
    return -1;
  if((dp = nameiparent(path, name)) == 0)
    return -1;

  begin_trans();

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }
  if(ip->type == T_PIPE && ip->read_file != 0){
    struct pipe* p = ip->read_file->pipe;
    acquire(&p->lock);
    p->is_deleted = 1;
    while(p->readopen > 0 && !proc->killed) {
      wakeup(&p->nread);
      sleep(&p->nwrite, &p->lock);
    }
    while(p->writeopen > 0 && !proc->killed) {
      wakeup(&p->nwrite);
      sleep(&p->nread, &p->lock);
    }
    release(&p->lock);
    // Because readopen and writeopen are both zeroes,
    // this fileclose will also close ip->write_open.
    iunlock(ip);
    fileclose(ip->read_file);
    ilock(ip);
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  commit_trans();

  return 0;

bad:
  iunlockput(dp);
  commit_trans();
  return -1;
}

static struct inode*
get_file(char *path)
{
  uint off;
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);

  if((ip = dirlookup(dp, name, &off)) != 0){
    iunlockput(dp);
    return ip;
  }

  iunlockput(dp);
  return 0;
}

static struct inode*
create(char *path, short type, short major, short minor, uint mode)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((ip = get_file(path)) != 0){
    ilock(ip);
    if((type == T_FILE && ip->type == T_FILE) ||
        (type == T_FILE && ip->type == T_PIPE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);
  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  ip->uid = proc->euid;
  ip->gid = proc->egid;
  ip->mode = (mode & (~(proc->umask)));
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

int
sys_open(void)
{
  char *path;
  int fd, omode, mode;
  struct file *f;
  struct inode *ip;

  if(argstr(0, &path) < 0 || argint(1, &omode) < 0)
    return -1;
  if(omode & O_CREATE && !get_file(path)){
    if(argint(2, &mode) < 0)
      return -1;
    begin_trans();
    ip = create(path, T_FILE, 0, 0, mode);
    commit_trans();
    if(ip == 0)
      return -1;
  } else {
    if((ip = namei(path)) == 0)
      return -1;
    ilock(ip);
    if((ip->type != T_PIPE) && (omode & O_NONBLOCK)){
      omode -= O_NONBLOCK;
    }
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      return -1;
    }
  }

  if(ip->type == T_PIPE){
    if(omode & O_RDWR){ // POSIX leaves this case undefined
      iunlockput(ip);
      return -1;
    }
    if(ip->read_file == 0){ // Create pipe if it has not been already created
      if(pipealloc(&ip->read_file, &ip->write_file) < 0){
        iunlockput(ip);
        return -1;
      }
      ip->read_file->type = FD_FIFO;
      ip->write_file->type = FD_FIFO;
      ip->read_file->ip = ip;
      ip->write_file->ip = ip;
      ip->read_file->pipe->writeopen = 0;
      ip->read_file->pipe->readopen = 0;
    }
    if((!(omode & O_WRONLY) && ((fd = fdalloc(ip->read_file)) < 0)) ||
        ((omode & O_WRONLY) && ((fd = fdalloc(ip->write_file)) < 0))){
      iunlockput(ip);
      return -1;
    }
    struct pipe* p = ip->write_file->pipe;
    acquire(&p->lock);
    if(omode & O_WRONLY){
      p->writeopen++;
      ip->write_file->ref++;
    } else {
      p->readopen++;
      ip->read_file->ref++;
    }
    iunlock(ip);
    if(omode & O_NONBLOCK){
      if((omode & O_WRONLY) && p->readopen == 0){
        release(&p->lock);
        return -1;
      }
      if(omode & O_WRONLY) wakeup(&p->nread);
      else wakeup(&p->nwrite);
      release(&p->lock);
      return fd;
    }
    // Wait ’till another end of pipe is open as POSIX requires to
    // when O_NONBLOCK is clear.
    int *our_end_count = &p->readopen, *other_end_count = &p->writeopen;
    uint *our_wakeup = &p->nread, *other_wakeup = &p->nwrite;
    if(omode & O_WRONLY){
      our_end_count = &p->writeopen;
      other_end_count = &p->readopen;
      our_wakeup = &p->nwrite;
      other_wakeup = &p->nread;
    }
    while (*other_end_count == 0){
      if(proc->killed || p->is_deleted){
        if(*our_end_count > 0) (*our_end_count)--;
        wakeup(other_wakeup);
        release(&p->lock);
        return -1;
      }
      wakeup(other_wakeup);
      sleep(our_wakeup, &p->lock);
    }
    wakeup(other_wakeup);
    release(&p->lock);
    return fd;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    return -1;
  }
  iunlock(ip);

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

int
sys_mkdir(void)
{
  char *path;
  struct inode *ip;
  int mode;
  if (argstr(0, &path) < 0 || argint(1, &mode) < 0){
    return -1;
  }

  begin_trans();
  if((ip = create(path, T_DIR, 0, 0, mode)) == 0){
    commit_trans();
    return -1;
  }
  iunlockput(ip);
  commit_trans();
  return 0;
}

int
sys_mknod(void)
{
  struct inode *ip;
  char *path;
  int len;
  int major, minor;
  int mode;
  
  begin_trans();
  if((len=argstr(0, &path)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     argint(3, &mode) < 0 ||
     (ip = create(path, T_DEV, major, minor, mode)) == 0){
    commit_trans();
    return -1;
  }
  iunlockput(ip);
  commit_trans();
  return 0;
}

int
sys_mkfifo(void)
{
  struct inode *ip;
  char* path;
  int mode;
  int len;
  begin_trans();
  if((len = argstr(0, &path)) < 0 ||
      argint(1, &mode) < 0 ||
      (ip = create(path, T_PIPE, 0, 0, mode)) == 0){
    commit_trans();
    return -1;
  }
  ip->read_file = ip->write_file = 0;
  iunlockput(ip);
  commit_trans();
  return 0;
}

int
sys_chdir(void)
{
  char *path;
  struct inode *ip;

  if(argstr(0, &path) < 0 || (ip = namei(path)) == 0)
    return -1;
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    return -1;
  }
  iunlock(ip);
  iput(proc->cwd);
  proc->cwd = ip;
  return 0;
}

int
sys_exec(void)
{
  char *path, *argv[MAXARG];
  int i;
  uint uargv, uarg;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv))
      return -1;
    if(fetchint(uargv+4*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    if(fetchstr(uarg, &argv[i]) < 0)
      return -1;
  }
  return exec(path, argv);
}

int
sys_pipe(void)
{
  int *fd;
  struct file *rf, *wf;
  int fd0, fd1;

  if(argptr(0, (void*)&fd, 2*sizeof(fd[0])) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      proc->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  fd[0] = fd0;
  fd[1] = fd1;
  return 0;
}

int
sys_umask()
{
  int new_value, old_value;
  if(argint(0, &new_value) < 0)
    return -1;
  old_value = proc->umask;
  proc->umask = new_value;
  return old_value;
}
