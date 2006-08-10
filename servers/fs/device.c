/* When a needed block is not in the cache, it must be fetched from the disk.
 * Special character files also require I/O.  The routines for these are here.
 *
 * The entry points in this file are:
 *   dev_open:   FS opens a device
 *   dev_close:  FS closes a device
 *   dev_io:	 FS does a read or write on a device
 *   dev_status: FS processes callback request alert
 *   gen_opcl:   generic call to a task to perform an open/close
 *   gen_io:     generic call to a task to perform an I/O operation
 *   no_dev:     open/close processing for devices that don't exist
 *   no_dev_io:  i/o processing for devices that don't exist
 *   tty_opcl:   perform tty-specific processing for open/close
 *   ctty_opcl:  perform controlling-tty-specific processing for open/close
 *   ctty_io:    perform controlling-tty-specific processing for I/O
 *   do_ioctl:	 perform the IOCTL system call
 *   pm_setsid:	 perform the SETSID system call (FS side)
 */

#include "fs.h"
#include <fcntl.h>
#include <assert.h>
#include <minix/callnr.h>
#include <minix/com.h>
#include <minix/endpoint.h>
#include <minix/ioctl.h>
#include <sys/ioc_tty.h>
#include "file.h"
#include "fproc.h"
#include "inode.h"
#include "param.h"
#include "super.h"

#define ELEMENTS(a) (sizeof(a)/sizeof((a)[0]))

FORWARD _PROTOTYPE( int safe_io_conversion, (endpoint_t,
  cp_grant_id_t *, int *, cp_grant_id_t *, int, endpoint_t *,
  void **, int *, vir_bytes, off_t *));
FORWARD _PROTOTYPE( void safe_io_cleanup, (cp_grant_id_t, cp_grant_id_t *,
	int));

extern int dmap_size;
PRIVATE int dummyproc;

/*===========================================================================*
 *				dev_open				     *
 *===========================================================================*/
PUBLIC int dev_open(dev, proc, flags)
dev_t dev;			/* device to open */
int proc;			/* process to open for */
int flags;			/* mode bits and flags */
{
  int major, r;
  struct dmap *dp;

  /* Determine the major device number call the device class specific
   * open/close routine.  (This is the only routine that must check the
   * device number for being in range.  All others can trust this check.)
   */
  major = (dev >> MAJOR) & BYTE;
  if (major >= NR_DEVICES) major = 0;
  dp = &dmap[major];
  if (dp->dmap_driver == NONE) 
	return ENXIO;
  r = (*dp->dmap_opcl)(DEV_OPEN, dev, proc, flags);
  if (r == SUSPEND) panic(__FILE__,"suspend on open from", dp->dmap_driver);
  return(r);
}

/*===========================================================================*
 *				dev_close				     *
 *===========================================================================*/
PUBLIC void dev_close(dev)
dev_t dev;			/* device to close */
{
  /* See if driver is roughly valid. */
  if (dmap[(dev >> MAJOR)].dmap_driver == NONE) {
	return;
  }
  (void) (*dmap[(dev >> MAJOR) & BYTE].dmap_opcl)(DEV_CLOSE, dev, 0, 0);
}

/*===========================================================================*
 *				suspended_ep				     *
 *===========================================================================*/
endpoint_t suspended_ep(endpoint_t driver, cp_grant_id_t g)
{
/* A process is suspended on a driver for which FS issued
 * a grant. Find out which process it was.
 */
	struct fproc *rfp;
	for (rfp = &fproc[0]; rfp < &fproc[NR_PROCS]; rfp++) {
		if(rfp->fp_pid == PID_FREE)
			continue;
		if(rfp->fp_suspended == SUSPENDED &&
		   rfp->fp_task == -driver && rfp->fp_grant == g) {
			return rfp->fp_endpoint;
		}
	}

	return NONE;
}

/*===========================================================================*
 *				dev_status				     *
 *===========================================================================*/
PUBLIC void dev_status(message *m)
{
	message st;
	int d, get_more = 1;
	endpoint_t endpt;

	for(d = 0; d < NR_DEVICES; d++)
		if (dmap[d].dmap_driver != NONE &&
		    dmap[d].dmap_driver == m->m_source)
			break;

	if (d >= NR_DEVICES)
		return;

	do {
		int r;
		st.m_type = DEV_STATUS;
		if ((r=sendrec(m->m_source, &st)) != OK) {
			printf("DEV_STATUS failed to %d: %d\n", m->m_source, r);
			if (r == EDEADSRCDST) return;
			if (r == EDSTDIED) return;
			if (r == ESRCDIED) return;
			panic(__FILE__,"couldn't sendrec for DEV_STATUS", r);
		}

		switch(st.m_type) {
			case DEV_REVIVE:
				endpt = st.REP_ENDPT;
				if(endpt == FS_PROC_NR) {
					endpt = suspended_ep(m->m_source,
						st.REP_IO_GRANT);
					if(endpt == NONE) {
						printf("FS: proc with "
					"grant %d from %d not found (revive)\n",
					st.m_source,
					st.REP_IO_GRANT);
						continue;
					}
				}
				revive(endpt, st.REP_STATUS);
				break;
			case DEV_IO_READY:
				select_notified(d, st.DEV_MINOR,
					st.DEV_SEL_OPS);
				break;
			default:
				printf("FS: unrecognized reply %d to "
					"DEV_STATUS\n", st.m_type);
				/* Fall through. */
			case DEV_NO_STATUS:
				get_more = 0;
				break;
		}
	} while(get_more);

	return;
}

/*===========================================================================*
 *				safe_io_conversion			     *
 *===========================================================================*/
PRIVATE int safe_io_conversion(driver, gid, op, gids, gids_size,
	io_ept, buf, vec_grants, bytes, pos)
endpoint_t driver;
cp_grant_id_t *gid;
int *op;
cp_grant_id_t *gids;
int gids_size;
endpoint_t *io_ept;
void **buf;
int *vec_grants;
vir_bytes bytes;
off_t *pos;
{
	int access = 0, size;
	int j;
	iovec_t *v;
	static iovec_t new_iovec[NR_IOREQS];

	/* Number of grants allocated in vector I/O. */
	*vec_grants = 0;

	/* Driver can handle it - change request to a safe one. */

	*gid = GRANT_INVALID;

	switch(*op) {
		case DEV_READ:
		case DEV_WRITE:
			/* Change to safe op. */
			*op = *op == DEV_READ ? DEV_READ_S : DEV_WRITE_S;

			if((*gid=cpf_grant_magic(driver, *io_ept,
			  (vir_bytes) *buf, bytes,
			  *op == DEV_READ_S ? CPF_WRITE : CPF_READ)) < 0) {
				panic(__FILE__,
				 "cpf_grant_magic of buffer failed\n", NO_NUM);
			}

			break;
		case DEV_GATHER:
		case DEV_SCATTER:
			/* Change to safe op. */
			*op = *op == DEV_GATHER ? DEV_GATHER_S : DEV_SCATTER_S;

			/* Grant access to my new i/o vector. */
			if((*gid = cpf_grant_direct(driver,
			  (vir_bytes) new_iovec, bytes * sizeof(iovec_t),
			  CPF_READ | CPF_WRITE)) < 0) {
				panic(__FILE__,
				"cpf_grant_direct of vector failed", NO_NUM);
			}
			v = (iovec_t *) *buf;
			/* Grant access to i/o buffers. */
			for(j = 0; j < bytes; j++) {
			   if(j >= NR_IOREQS) 
				panic(__FILE__, "vec too big", bytes);
			   new_iovec[j].iov_addr = gids[j] =
			     cpf_grant_direct(driver, (vir_bytes)
			     v[j].iov_addr, v[j].iov_size,
			     *op == DEV_GATHER_S ? CPF_WRITE : CPF_READ);
			   if(!GRANT_VALID(gids[j])) {
				panic(__FILE__, "grant to iovec buf failed",
				 NO_NUM);
			   }
			   new_iovec[j].iov_size = v[j].iov_size;
			   (*vec_grants)++;
			}

			/* Set user's vector to the new one. */
			*buf = new_iovec;
			break;
		case DEV_IOCTL:
			*pos = *io_ept;	/* Old endpoint in POSITION field. */
			*op = DEV_IOCTL_S;
			if(_MINIX_IOCTL_IOR(m_in.REQUEST)) access |= CPF_WRITE;
			if(_MINIX_IOCTL_IOW(m_in.REQUEST)) access |= CPF_READ;
			if(_MINIX_IOCTL_BIG(m_in.REQUEST))
				size = _MINIX_IOCTL_SIZE_BIG(m_in.REQUEST);
			else
				size = _MINIX_IOCTL_SIZE(m_in.REQUEST);

			/* Do this even if no I/O happens with the ioctl, in
			 * order to disambiguate requests with DEV_IOCTL_S.
			 */
			if((*gid=cpf_grant_magic(driver, *io_ept,
				(vir_bytes) *buf, size, access)) < 0) {
				panic(__FILE__,
				"cpf_grant_magic failed (ioctl)\n",
				NO_NUM);
			}
	}

	/* If we have converted to a safe operation, I/O
	 * endpoint becomes FS if it wasn't already.
	 */
	if(GRANT_VALID(*gid)) {
		*io_ept = FS_PROC_NR;
		return 1;
	}

	/* Not converted to a safe operation (because there is no
	 * copying involved in this operation).
	 */
	return 0;
}

/*===========================================================================*
 *			safe_io_cleanup					     *
 *===========================================================================*/
PRIVATE void safe_io_cleanup(gid, gids, gids_size)
cp_grant_id_t gid;
cp_grant_id_t *gids;
int gids_size;
{
/* Free resources (specifically, grants) allocated by safe_io_conversion(). */
	int j;

  	cpf_revoke(gid);

	for(j = 0; j < gids_size; j++)
		cpf_revoke(gids[j]);

	return;
}

/*===========================================================================*
 *				dev_bio					     *
 *===========================================================================*/
PUBLIC int dev_bio(op, dev, proc_e, buf, pos, bytes)
int op;				/* DEV_READ, DEV_WRITE, DEV_IOCTL, etc. */
dev_t dev;			/* major-minor device number */
int proc_e;			/* in whose address space is buf? */
void *buf;			/* virtual address of the buffer */
off_t pos;			/* byte position */
int bytes;			/* how many bytes to transfer */
{
/* Read or write from a device.  The parameter 'dev' tells which one. */
  struct dmap *dp;
  int r, safe;
  message m;
  iovec_t *v;
  cp_grant_id_t gid = GRANT_INVALID;
	int vec_grants;

  /* Determine task dmap. */
  dp = &dmap[(dev >> MAJOR) & BYTE];

  /* The io vector copying relies on this I/O being for FS itself. */
  if(proc_e != FS_PROC_NR)
	panic(__FILE__, "doing dev_bio for non-self", proc_e);

  for (;;)
  {
	int op_used;
	void *buf_used;
        static cp_grant_id_t gids[NR_IOREQS];
        cp_grant_id_t gid = GRANT_INVALID;
	int vec_grants;

	/* See if driver is roughly valid. */
	if (dp->dmap_driver == NONE) {
		printf("FS: dev_io: no driver for dev %x\n", dev);
		return ENXIO;
	}

        /* By default, these are right. */
	m.IO_ENDPT = proc_e;
	m.ADDRESS  = buf;
	buf_used = buf;

	/* Convert parameters to 'safe mode'. */
	op_used = op;
        safe = safe_io_conversion(dp->dmap_driver, &gid,
          &op_used, gids, NR_IOREQS, &m.IO_ENDPT, &buf_used,
	  &vec_grants, bytes, &pos);

	/* Set up rest of the message. */
	if(safe) m.IO_GRANT = (char *) gid;

	m.m_type   = op_used;
	m.DEVICE   = (dev >> MINOR) & BYTE;
	m.POSITION = pos;
	m.COUNT    = bytes;
	m.HIGHPOS  = 0;

	/* Call the task. */
	(*dp->dmap_io)(dp->dmap_driver, &m);

	/* As block I/O never SUSPENDs, safe cleanup must be done whether
	 * the I/O succeeded or not.
	 */
	if(safe) safe_io_cleanup(gid, gids, vec_grants);

	if(dp->dmap_driver == NONE) {
		/* Driver has vanished. Wait for a new one. */
		for (;;)
		{
			r= receive(RS_PROC_NR, &m);
			if (r != OK)
			{
				panic(__FILE__,
					"dev_bio: unable to receive from RS",
					r);
			}
			if (m.m_type == DEVCTL)
			{
				r= fs_devctl(m.ctl_req, m.dev_nr, m.driver_nr,
					m.dev_style, m.m_force);
			}
			else
			{
				panic(__FILE__,
					"dev_bio: got message from RS, type",
					m.m_type);
			}
			m.m_type= r;
			r= send(RS_PROC_NR, &m);
			if (r != OK)
			{
				panic(__FILE__,
					"dev_bio: unable to send to RS",
					r);
			}
			if (dp->dmap_driver != NONE)
				break;
		}
		printf("dev_bio: trying new driver\n");
		continue;
	}

	/* Task has completed.  See if call completed. */
	if (m.REP_STATUS == SUSPEND) {
		panic(__FILE__, "dev_bio: driver returned SUSPEND", NO_NUM);
	}

	if(buf != buf_used) {
		memcpy(buf, buf_used, bytes * sizeof(iovec_t));
	}

	return(m.REP_STATUS);
  }
}

/*===========================================================================*
 *				dev_io					     *
 *===========================================================================*/
PUBLIC int dev_io(op, dev, proc_e, buf, pos, bytes, flags)
int op;				/* DEV_READ, DEV_WRITE, DEV_IOCTL, etc. */
dev_t dev;			/* major-minor device number */
int proc_e;			/* in whose address space is buf? */
void *buf;			/* virtual address of the buffer */
off_t pos;			/* byte position */
int bytes;			/* how many bytes to transfer */
int flags;			/* special flags, like O_NONBLOCK */
{
/* Read or write from a device.  The parameter 'dev' tells which one. */
  struct dmap *dp;
  message dev_mess;
  cp_grant_id_t gid = GRANT_INVALID;
  static cp_grant_id_t gids[NR_IOREQS];
  int vec_grants = 0, orig_op, safe;
  void *buf_used;
  endpoint_t ioproc;

  /* Determine task dmap. */
  dp = &dmap[(dev >> MAJOR) & BYTE];
  orig_op = op;

  /* See if driver is roughly valid. */
  if (dp->dmap_driver == NONE) {
	printf("FS: dev_io: no driver for dev %x\n", dev);
	return ENXIO;
  }

  if(isokendpt(dp->dmap_driver, &dummyproc) != OK) {
	printf("FS: dev_io: old driver for dev %x (%d)\n",
		dev, dp->dmap_driver);
	return ENXIO;
  }

  /* By default, these are right. */
  dev_mess.IO_ENDPT = proc_e;
  dev_mess.ADDRESS  = buf;

  /* Convert DEV_* to DEV_*_S variants. */
  buf_used = buf;
  safe = safe_io_conversion(dp->dmap_driver, &gid,
    &op, gids, NR_IOREQS, &dev_mess.IO_ENDPT, &buf_used,
    &vec_grants, bytes, &pos);

  if(buf != buf_used)
	panic(__FILE__,"dev_io: safe_io_conversion changed buffer", NO_NUM);

  /* If the safe conversion was done, set the ADDRESS to
   * the grant id.
   */
  if(safe) dev_mess.IO_GRANT = (char *) gid;

  /* Set up the rest of the message passed to task. */
  dev_mess.m_type   = op;
  dev_mess.DEVICE   = (dev >> MINOR) & BYTE;
  dev_mess.POSITION = pos;
  dev_mess.COUNT    = bytes;
  dev_mess.HIGHPOS  = 0;

  /* This will be used if the i/o is suspended. */
  ioproc = dev_mess.IO_ENDPT;

  /* Call the task. */
  (*dp->dmap_io)(dp->dmap_driver, &dev_mess);

  if(dp->dmap_driver == NONE) {
  	/* Driver has vanished. */
	printf("Driver gone?\n");
	if(safe) safe_io_cleanup(gid, gids, vec_grants);
	return EIO;
  }

  /* Task has completed.  See if call completed. */
  if (dev_mess.REP_STATUS == SUSPEND) {
	if(vec_grants > 0) {
		panic(__FILE__,"SUSPEND on vectored i/o", NO_NUM);
	}
	/* fp is uninitialized at init time. */
	if(!fp)
		panic(__FILE__,"SUSPEND on NULL fp", NO_NUM);
	if (flags & O_NONBLOCK) {
		/* Not supposed to block. */
		dev_mess.m_type = CANCEL;
		dev_mess.IO_ENDPT = ioproc;
		dev_mess.IO_GRANT = (char *) gid;

		/* This R_BIT/W_BIT check taken from suspend()/unpause()
		 * logic. Mode is expected in the COUNT field.
		 */
		dev_mess.COUNT = 0;
		if(call_nr == READ) 		dev_mess.COUNT = R_BIT;
		else if(call_nr == WRITE)	dev_mess.COUNT = W_BIT;
		dev_mess.DEVICE = (dev >> MINOR) & BYTE;
		(*dp->dmap_io)(dp->dmap_driver, &dev_mess);
		if (dev_mess.REP_STATUS == EINTR) dev_mess.REP_STATUS = EAGAIN;
	} else {
		/* Suspend user. */
		suspend(dp->dmap_driver);
		assert(!GRANT_VALID(fp->fp_grant));
		fp->fp_grant = gid;	/* revoke this when unsuspended. */
		fp->fp_ioproc = ioproc;
		return(SUSPEND);
	}
  }

  /* No suspend, or cancelled suspend, so I/O is over and can be cleaned up. */
  if(safe) safe_io_cleanup(gid, gids, vec_grants);

  return(dev_mess.REP_STATUS);
}

/*===========================================================================*
 *				gen_opcl				     *
 *===========================================================================*/
PUBLIC int gen_opcl(op, dev, proc_e, flags)
int op;				/* operation, DEV_OPEN or DEV_CLOSE */
dev_t dev;			/* device to open or close */
int proc_e;			/* process to open/close for */
int flags;			/* mode bits and flags */
{
/* Called from the dmap struct in table.c on opens & closes of special files.*/
  struct dmap *dp;
  message dev_mess;

  /* Determine task dmap. */
  dp = &dmap[(dev >> MAJOR) & BYTE];

  dev_mess.m_type   = op;
  dev_mess.DEVICE   = (dev >> MINOR) & BYTE;
  dev_mess.IO_ENDPT = proc_e;
  dev_mess.COUNT    = flags;

  if (dp->dmap_driver == NONE) {
	printf("FS: gen_opcl: no driver for dev %x\n", dev);
	return ENXIO;
  }

  /* Call the task. */
  (*dp->dmap_io)(dp->dmap_driver, &dev_mess);

  return(dev_mess.REP_STATUS);
}

/*===========================================================================*
 *				tty_opcl				     *
 *===========================================================================*/
PUBLIC int tty_opcl(op, dev, proc_e, flags)
int op;				/* operation, DEV_OPEN or DEV_CLOSE */
dev_t dev;			/* device to open or close */
int proc_e;			/* process to open/close for */
int flags;			/* mode bits and flags */
{
/* This procedure is called from the dmap struct on tty open/close. */
 
  int r;
  register struct fproc *rfp;

  /* Add O_NOCTTY to the flags if this process is not a session leader, or
   * if it already has a controlling tty, or if it is someone elses
   * controlling tty.
   */
  if (!fp->fp_sesldr || fp->fp_tty != 0) {
	flags |= O_NOCTTY;
  } else {
	for (rfp = &fproc[0]; rfp < &fproc[NR_PROCS]; rfp++) {
		if(rfp->fp_pid == PID_FREE) continue;
		if (rfp->fp_tty == dev) flags |= O_NOCTTY;
	}
  }

  r = gen_opcl(op, dev, proc_e, flags);

  /* Did this call make the tty the controlling tty? */
  if (r == 1) {
	fp->fp_tty = dev;
	r = OK;
  }
  return(r);
}

/*===========================================================================*
 *				ctty_opcl				     *
 *===========================================================================*/
PUBLIC int ctty_opcl(op, dev, proc_e, flags)
int op;				/* operation, DEV_OPEN or DEV_CLOSE */
dev_t dev;			/* device to open or close */
int proc_e;			/* process to open/close for */
int flags;			/* mode bits and flags */
{
/* This procedure is called from the dmap struct in table.c on opening/closing
 * /dev/tty, the magic device that translates to the controlling tty.
 */
 
  return(fp->fp_tty == 0 ? ENXIO : OK);
}

/*===========================================================================*
 *				pm_setsid				     *
 *===========================================================================*/
PUBLIC void pm_setsid(proc_e)
int proc_e;
{
/* Perform the FS side of the SETSID call, i.e. get rid of the controlling
 * terminal of a process, and make the process a session leader.
 */
  register struct fproc *rfp;
  int slot;

  /* Make the process a session leader with no controlling tty. */
  okendpt(proc_e, &slot);
  rfp = &fproc[slot];
  rfp->fp_sesldr = TRUE;
  rfp->fp_tty = 0;
}

/*===========================================================================*
 *				do_ioctl				     *
 *===========================================================================*/
PUBLIC int do_ioctl()
{
/* Perform the ioctl(ls_fd, request, argx) system call (uses m2 fmt). */

  struct filp *f;
  register struct inode *rip;
  dev_t dev;

  if ( (f = get_filp(m_in.ls_fd)) == NIL_FILP) return(err_code);
  rip = f->filp_ino;		/* get inode pointer */
  if ( (rip->i_mode & I_TYPE) != I_CHAR_SPECIAL
	&& (rip->i_mode & I_TYPE) != I_BLOCK_SPECIAL) return(ENOTTY);
  dev = (dev_t) rip->i_zone[0];

  return(dev_io(DEV_IOCTL, dev, who_e, m_in.ADDRESS, 0L, 
  	m_in.REQUEST, f->filp_flags));
}

/*===========================================================================*
 *				gen_io					     *
 *===========================================================================*/
PUBLIC int gen_io(task_nr, mess_ptr)
int task_nr;			/* which task to call */
message *mess_ptr;		/* pointer to message for task */
{
/* All file system I/O ultimately comes down to I/O on major/minor device
 * pairs.  These lead to calls on the following routines via the dmap table.
 */

  int r, proc_e;

  proc_e = mess_ptr->IO_ENDPT;

  r = sendrec(task_nr, mess_ptr);
	if (r != OK) {
		if (r == EDEADSRCDST || r == EDSTDIED || r == ESRCDIED) {
			printf("fs: dead driver %d\n", task_nr);
			dmap_unmap_by_endpt(task_nr);
			return r;
		}
		if (r == ELOCKED) {
			printf("fs: ELOCKED talking to %d\n", task_nr);
			return r;
		}
		panic(__FILE__,"call_task: can't send/receive", r);
	}

  	/* Did the process we did the sendrec() for get a result? */
  	if (mess_ptr->REP_ENDPT != proc_e) {
		printf(
		"fs: strange device reply from %d, type = %d, proc = %d (not %d) (2) ignored\n",
			mess_ptr->m_source,
			mess_ptr->m_type,
			proc_e,
			mess_ptr->REP_ENDPT);
		return EIO;
	}

  return OK;
}

/*===========================================================================*
 *				ctty_io					     *
 *===========================================================================*/
PUBLIC int ctty_io(task_nr, mess_ptr)
int task_nr;			/* not used - for compatibility with dmap_t */
message *mess_ptr;		/* pointer to message for task */
{
/* This routine is only called for one device, namely /dev/tty.  Its job
 * is to change the message to use the controlling terminal, instead of the
 * major/minor pair for /dev/tty itself.
 */

  struct dmap *dp;

  if (fp->fp_tty == 0) {
	/* No controlling tty present anymore, return an I/O error. */
	mess_ptr->REP_STATUS = EIO;
  } else {
	/* Substitute the controlling terminal device. */
	dp = &dmap[(fp->fp_tty >> MAJOR) & BYTE];
	mess_ptr->DEVICE = (fp->fp_tty >> MINOR) & BYTE;

  if (dp->dmap_driver == NONE) {
	printf("FS: ctty_io: no driver for dev\n");
	return EIO;
  }

	if(isokendpt(dp->dmap_driver, &dummyproc) != OK) {
		printf("FS: ctty_io: old driver %d\n",
			dp->dmap_driver);
		return EIO;
	}

	(*dp->dmap_io)(dp->dmap_driver, mess_ptr);
  }
  return OK;
}

/*===========================================================================*
 *				no_dev					     *
 *===========================================================================*/
PUBLIC int no_dev(op, dev, proc, flags)
int op;				/* operation, DEV_OPEN or DEV_CLOSE */
dev_t dev;			/* device to open or close */
int proc;			/* process to open/close for */
int flags;			/* mode bits and flags */
{
/* Called when opening a nonexistent device. */
  return(ENODEV);
}

/*===========================================================================*
 *				no_dev_io				     *
 *===========================================================================*/
PUBLIC int no_dev_io(int proc, message *m)
{
/* Called when doing i/o on a nonexistent device. */
  printf("FS: I/O on unmapped device number\n");
  return EIO;
}

/*===========================================================================*
 *				clone_opcl				     *
 *===========================================================================*/
PUBLIC int clone_opcl(op, dev, proc_e, flags)
int op;				/* operation, DEV_OPEN or DEV_CLOSE */
dev_t dev;			/* device to open or close */
int proc_e;			/* process to open/close for */
int flags;			/* mode bits and flags */
{
/* Some devices need special processing upon open.  Such a device is "cloned",
 * i.e. on a succesful open it is replaced by a new device with a new unique
 * minor device number.  This new device number identifies a new object (such
 * as a new network connection) that has been allocated within a task.
 */
  struct dmap *dp;
  int r, minor;
  message dev_mess;

  /* Determine task dmap. */
  dp = &dmap[(dev >> MAJOR) & BYTE];
  minor = (dev >> MINOR) & BYTE;

  dev_mess.m_type   = op;
  dev_mess.DEVICE   = minor;
  dev_mess.IO_ENDPT = proc_e;
  dev_mess.COUNT    = flags;


  if (dp->dmap_driver == NONE) {
	printf("FS: clone_opcl: no driver for dev %x\n", dev);
	return ENXIO;
  }

  if(isokendpt(dp->dmap_driver, &dummyproc) != OK) {
  	printf("FS: clone_opcl: old driver for dev %x (%d)\n",
  		dev, dp->dmap_driver);
  	return ENXIO;
  }

  /* Call the task. */
  r= (*dp->dmap_io)(dp->dmap_driver, &dev_mess);
  if (r != OK)
	return r;

  if (op == DEV_OPEN && dev_mess.REP_STATUS >= 0) {
	if (dev_mess.REP_STATUS != minor) {
		/* A new minor device number has been returned.  Create a
		 * temporary device file to hold it.
		 */
		struct inode *ip;

		/* Device number of the new device. */
		dev = (dev & ~(BYTE << MINOR)) | (dev_mess.REP_STATUS << MINOR);

		ip = alloc_inode(root_dev, ALL_MODES | I_CHAR_SPECIAL);
		if (ip == NIL_INODE) {
			/* Oops, that didn't work.  Undo open. */
			(void) clone_opcl(DEV_CLOSE, dev, proc_e, 0);
			return(err_code);
		}
		ip->i_zone[0] = dev;

		put_inode(fp->fp_filp[m_in.fd]->filp_ino);
		fp->fp_filp[m_in.fd]->filp_ino = ip;
	}
	dev_mess.REP_STATUS = OK;
  }
  return(dev_mess.REP_STATUS);
}

/*===========================================================================*
 *				dev_up					     *
 *===========================================================================*/
PUBLIC void dev_up(int maj)
{
	/* A new device driver has been mapped in. This function
	 * checks if any filesystems are mounted on it, and if so,
	 * dev_open()s them so the filesystem can be reused.
	 */
	struct super_block *sb;
	struct filp *fp;
	int r;

	/* Open a device once for every filp that's opened on it,
	 * and once for every filesystem mounted from it.
	 */

	for(sb = super_block; sb < &super_block[NR_SUPERS]; sb++) {
		int minor;
		if(sb->s_dev == NO_DEV)
			continue;
		if(((sb->s_dev >> MAJOR) & BYTE) != maj)
			continue;
		minor = ((sb->s_dev >> MINOR) & BYTE);
		printf("FS: remounting dev %d/%d\n", maj, minor);
		if((r = dev_open(sb->s_dev, FS_PROC_NR,
		   sb->s_rd_only ? R_BIT : (R_BIT|W_BIT))) != OK) {
			printf("FS: mounted dev %d/%d re-open failed: %d.\n",
				maj, minor, r);
		}
	}

	for(fp = filp; fp < &filp[NR_FILPS]; fp++) {
		struct inode *in;
		int minor;

		if(fp->filp_count < 1 || !(in=fp->filp_ino)) continue;
		if(((in->i_zone[0] >> MAJOR) & BYTE) != maj) continue;
		if(!(in->i_mode & (I_BLOCK_SPECIAL|I_CHAR_SPECIAL))) continue;
		
		minor = ((in->i_zone[0] >> MINOR) & BYTE);

		printf("FS: reopening special %d/%d..\n", maj, minor);

		if((r = dev_open(in->i_zone[0], FS_PROC_NR,
		   in->i_mode & (R_BIT|W_BIT))) != OK) {
			int n;
			/* This function will set the fp_filp[]s of processes
			 * holding that fp to NULL, but _not_ clear
			 * fp_filp_inuse, so that fd can't be recycled until
			 * it's close()d.
			 */
			n = inval_filp(fp);
			if(n != fp->filp_count)
				printf("FS: warning: invalidate/count "
				 "discrepancy (%d, %d)\n", n, fp->filp_count);
			fp->filp_count = 0;
			printf("FS: file on dev %d/%d re-open failed: %d; "
				"invalidated %d fd's.\n", maj, minor, r, n);
		}
	}

	return;
}

