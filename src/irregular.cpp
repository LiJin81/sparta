/* ----------------------------------------------------------------------
   SPARTA - Stochastic PArallel Rarefied-gas Time-accurate Analyzer
   http://sparta.sandia.gov
   Steve Plimpton, sjplimp@sandia.gov, Michael Gallis, magalli@sandia.gov
   Sandia National Laboratories

   Copyright (2014) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level SPARTA directory.
------------------------------------------------------------------------- */

#include "spatype.h"
#include "mpi.h"
#include "stdlib.h"
#include "string.h"
#include "irregular.h"
#include "particle.h"
#include "domain.h"
#include "comm.h"
#include "memory.h"
#include "error.h"



// DEBUG
#include "update.h"
#include "grid.h"

using namespace SPARTA_NS;

// allocate space for static class variable
// prototype for non-class function

int *Irregular::proc_recv_copy;
int compare_standalone(const void *, const void *);

#define BUFFACTOR 1.5
#define BUFMIN 1000
#define BUFEXTRA 1000

/* ---------------------------------------------------------------------- */

Irregular::Irregular(SPARTA *sparta) : Pointers(sparta)
{
  MPI_Comm_rank(world,&me);
  MPI_Comm_size(world,&nprocs);

  // allocate fixed-length and work vectors for plan

  memory->create(proc_send,nprocs,"irregular:proc_send");
  memory->create(num_send,nprocs,"irregular:num_send");
  memory->create(proc_recv,nprocs,"irregular:proc_recv");
  memory->create(num_recv,nprocs,"irregular:num_recv");
  memory->create(proc2recv,nprocs,"irregular:proc2recv");

  request = new MPI_Request[nprocs];
  status = new MPI_Status[nprocs];

  size_send = NULL;
  size_recv = NULL;

  memory->create(work1,nprocs,"irregular:work1");
  memory->create(work2,nprocs,"irregular:work2");

  indexmax = 0;
  index_send = NULL;
  indexselfmax = 0;
  index_self = NULL;
  offsetmax = 0;
  offset_send = NULL;
  bufmax = 0;
  buf = NULL;
}

/* ---------------------------------------------------------------------- */

Irregular::~Irregular()
{
  memory->destroy(proc_send);
  memory->destroy(num_send);
  memory->destroy(proc_recv);
  memory->destroy(num_recv);
  memory->destroy(proc2recv);
  delete [] request;
  delete [] status;
  memory->destroy(size_send);
  memory->destroy(size_recv);
  memory->destroy(work1);
  memory->destroy(work2);
  memory->destroy(index_send);
  memory->destroy(index_self);
  memory->destroy(offset_send);
  memory->destroy(buf);
}

/* ----------------------------------------------------------------------
   create communication plan based on list of procs to possibly send to
   n = # of procs I possibly send to, self is ignored if included
   proclist = list of the procs
   only sets nsend, nrecv, proc_send, proc_recv, proc2recv
     other plan fields are set by augment_data()
     also sets up work1 vector for augment_data() to use
------------------------------------------------------------------------- */

void Irregular::create_procs(int n, int *proclist, int sort)
{
  int i,m;

  // setup for collective comm
  // work1 = 1 for procs I send to, set self to 0
  // work2 = 1 for all procs, used for ReduceScatter
  // nsend = # of procs I send messages to, not including self

  for (i = 0; i < nprocs; i++) {
    work1[i] = 0;
    work2[i] = 1;
  }
  for (i = 0; i < n; i++) work1[proclist[i]] = 1;

  nsend = n;
  if (work1[me]) {
    work1[me] = 0;
    nsend--;
  }

  // nrecv = # of procs I receive messages from, not including self
  // options for performing ReduceScatter operation
  // some are more efficient on some machines at big sizes

#ifdef SPARTA_RS_ALLREDUCE_INPLACE
  MPI_Allreduce(MPI_IN_PLACE,work1,nprocs,MPI_INT,MPI_SUM,world);
  nrecv = work1[me];
#else 
#ifdef SPARTA_RS_ALLREDUCE
  MPI_Allreduce(work1,work2,nprocs,MPI_INT,MPI_SUM,world);
  nrecv = work2[me];
#else
  MPI_Reduce_scatter(work1,&nrecv,work2,MPI_INT,MPI_SUM,world);
#endif
#endif

  // proc_send = procs I send to
  // to balance pattern of send messages:
  //   each proc starts with iproc > me, continues until iproc = me
  // reset work1 to store which send message each proc corresponds to
  //   used by augmen_data()

  for (i = 0; i < nprocs; i++) work1[i] = 0;
  for (i = 0; i < n; i++) work1[proclist[i]] = 1;
  work1[me] = 0;

  int iproc = me;
  int isend = 0;
  for (i = 0; i < nprocs; i++) {
    iproc++;
    if (iproc == nprocs) iproc = 0;
    if (iproc == me) continue;
    if (work1[iproc]) {
      proc_send[isend] = iproc;
      work1[iproc] = isend;
      isend++;
    }
  }

  // tell receivers I send to them

  m = 0;
  for (i = 0; i < nsend; i++)
    MPI_Send(&m,0,MPI_INT,proc_send[i],0,world);

  // receive incoming messages
  // proc_recv = procs I recv from

  for (i = 0; i < nrecv; i++) {
    MPI_Recv(&m,0,MPI_INT,MPI_ANY_SOURCE,0,world,status);
    proc_recv[i] = status->MPI_SOURCE;
  }

  // sort proc_recv by proc ID if requested
  // useful for debugging to insure reproducible ordering of received datums

  if (sort) {
    int *order = new int[nrecv];
    int *proc_recv_ordered = new int[nrecv];

    for (i = 0; i < nrecv; i++) order[i] = i;
    proc_recv_copy = proc_recv;
    qsort(order,nrecv,sizeof(int),compare_standalone);

    int j;
    for (i = 0; i < nrecv; i++) {
      j = order[i];
      proc_recv_ordered[i] = proc_recv[j];
    }

    memcpy(proc_recv,proc_recv_ordered,nrecv*sizeof(int));
    delete [] order;
    delete [] proc_recv_ordered;
  }

  // proc2recv[I] = which recv the Ith proc ID is
  // will only be accessed by procs I actually receive from

  for (i = 0; i < nrecv; i++) proc2recv[proc_recv[i]] = i;

  // barrier to insure all MPI_ANY_SOURCE messages are received
  // else another proc could proceed to augment_data() and send to me

  MPI_Barrier(world);
}

/* ----------------------------------------------------------------------
   create communication plan based on list of datums of uniform size
   n = # of datums to send
   proclist = proc to send each datum to, can include self
   sort = flag for sorting order of received datums by proc ID
   return total # of datums I will recv, including any to self
------------------------------------------------------------------------- */

int Irregular::create_data_uniform(int n, int *proclist, int sort)
{
  int i,m;

  // setup for collective comm
  // work1 = # of datums I send to each proc, set self to 0
  // work2 = 1 for all procs, used for ReduceScatter

  for (i = 0; i < nprocs; i++) {
    work1[i] = 0;
    work2[i] = 1;
  }
  for (i = 0; i < n; i++) work1[proclist[i]] = 1;
  work1[me] = 0;

  // nrecv = # of procs I receive messages from, not including self
  // options for performing ReduceScatter operation
  // some are more efficient on some machines at big sizes

#ifdef SPARTA_RS_ALLREDUCE_INPLACE
  MPI_Allreduce(MPI_IN_PLACE,work1,nprocs,MPI_INT,MPI_SUM,world);
  nrecv = work1[me];
#else 
#ifdef SPARTA_RS_ALLREDUCE
  MPI_Allreduce(work1,work2,nprocs,MPI_INT,MPI_SUM,world);
  nrecv = work2[me];
#else
  MPI_Reduce_scatter(work1,&nrecv,work2,MPI_INT,MPI_SUM,world);
#endif
#endif

  // work1 = # of datums I send to each proc, including self
  // nsend = # of procs I send messages to, not including self

  for (i = 0; i < nprocs; i++) work1[i] = 0;
  for (i = 0; i < n; i++) work1[proclist[i]]++;

  nsend = 0;
  for (i = 0; i < nprocs; i++)
    if (work1[i]) nsend++;
  if (work1[me]) nsend--;

  // reallocate send and self index lists if necessary
  // could use n-work1[me] for length of index_send to be more precise

  if (n > indexmax) {
    indexmax = n;
    memory->destroy(index_send);
    memory->create(index_send,indexmax,"irregular:index_send");
  }

  if (work1[me] > indexselfmax) {
    indexselfmax = work1[me];
    memory->destroy(index_self);
    memory->create(index_self,indexselfmax,"irregular:index_self");
  }

  // proc_send = procs I send to
  // num_send = # of datums I send to each proc
  // num_self = # of datums I copy to self
  // to balance pattern of send messages:
  //   each proc starts with iproc > me, continues until iproc = me
  // reset work1 to store which send message each proc corresponds to

  int iproc = me;
  int isend = 0;
  for (i = 0; i < nprocs; i++) {
    iproc++;
    if (iproc == nprocs) iproc = 0;
    if (iproc == me) {
      num_self = work1[iproc];
      work1[iproc] = 0;
    } else if (work1[iproc]) {
      proc_send[isend] = iproc;
      num_send[isend] = work1[iproc];
      work1[iproc] = isend;
      isend++;
    }
  }

  // work2 = offsets into index_send for each proc I send to
  // m = ptr into index_self
  // index_send = list of which datums to send to each proc
  //   1st N1 values are datum indices for 1st proc,
  //   next N2 values are datum indices for 2nd proc, etc
  // index_self = list of which datums to copy to self

  work2[0] = 0;
  for (i = 1; i < nsend; i++) work2[i] = work2[i-1] + num_send[i-1];

  m = 0;
  for (i = 0; i < n; i++) {
    iproc = proclist[i];
    if (iproc == me) index_self[m++] = i;
    else {
      isend = work1[iproc];
      index_send[work2[isend]++] = i;
    }
  }

  // tell receivers how many datums I send them
  // sendmax = largest # of datums I send in a single message

  sendmax = 0;
  for (i = 0; i < nsend; i++) {
    MPI_Send(&num_send[i],1,MPI_INT,proc_send[i],0,world);
    sendmax = MAX(sendmax,num_send[i]);
  }

  // receive incoming messages
  // proc_recv = procs I recv from
  // num_recv = # of datums each proc sends me
  // nrecvdatum = total # of datums I recv

  nrecvdatum = 0;
  for (i = 0; i < nrecv; i++) {
    MPI_Recv(&num_recv[i],1,MPI_INT,MPI_ANY_SOURCE,0,world,status);
    proc_recv[i] = status->MPI_SOURCE;
    nrecvdatum += num_recv[i];
  }
  nrecvdatum += num_self;

  // sort proc_recv and num_recv by proc ID if requested
  // useful for debugging to insure reproducible ordering of received datums

  if (sort) {
    int *order = new int[nrecv];
    int *proc_recv_ordered = new int[nrecv];
    int *num_recv_ordered = new int[nrecv];

    for (i = 0; i < nrecv; i++) order[i] = i;
    proc_recv_copy = proc_recv;
    qsort(order,nrecv,sizeof(int),compare_standalone);

    int j;
    for (i = 0; i < nrecv; i++) {
      j = order[i];
      proc_recv_ordered[i] = proc_recv[j];
      num_recv_ordered[i] = num_recv[j];
    }

    memcpy(proc_recv,proc_recv_ordered,nrecv*sizeof(int));
    memcpy(num_recv,num_recv_ordered,nrecv*sizeof(int));
    delete [] order;
    delete [] proc_recv_ordered;
    delete [] num_recv_ordered;
  }

  // proc2recv[I] = which recv the Ith proc ID is
  // will only be accessed by procs I actually receive from

  for (i = 0; i < nrecv; i++) proc2recv[proc_recv[i]] = i;

  // barrier to insure all MPI_ANY_SOURCE messages are received
  // else another proc could proceed to exchange_data() and send to me

  MPI_Barrier(world);

  // return # of datums I will receive

  return nrecvdatum;
}

/* ----------------------------------------------------------------------
   comparison function invoked by qsort()
   accesses static class member proc_recv_copy, set before call to qsort()
------------------------------------------------------------------------- */

int compare_standalone(const void *iptr, const void *jptr)
{
  int i = *((int *) iptr);
  int j = *((int *) jptr);
  int *proc_recv = Irregular::proc_recv_copy;
  if (proc_recv[i] < proc_recv[j]) return -1;
  if (proc_recv[i] > proc_recv[j]) return 1;
  return 0;
}

/* ----------------------------------------------------------------------
   create communication plan based on list of datums of variable size
   n = # of datums to send
   proclist = proc to send each datum to, can include self
   sizes = size of each datum in bytes
   sort = flag for sorting order of received datums by proc ID
   return total # of datums I will recv, including any to self
   return recvsize = total byte size of datums I will receive, including self
------------------------------------------------------------------------- */

int Irregular::create_data_variable(int n, int *proclist, int *sizes,
                                    int &recvsize, int sort)
{
  int i;

  int nrecvdatum = create_data_uniform(n,proclist,sort);

  // allocate size_send and size_recv for first time if necessary

  if (!size_send) {
    memory->create(size_send,nprocs,"irregular:size_send");
    memory->create(size_recv,nprocs,"irregular:size_recv");
  }

  // reallocate offset list if necessary

  if (n > offsetmax) {
    offsetmax = n;
    memory->destroy(offset_send);
    memory->create(offset_send,offsetmax,"irregular:offset_send");
  }

  // offset_send = byte offset for each send datum

  int offset = 0;
  for (i = 0; i < n; i++) {
    offset_send[i] = offset;
    offset += sizes[i];
  }

  // work1 = # of bytes to send to each proc, including self

  for (i = 0; i < nprocs; i++) work1[i] = 0;
  for (i = 0; i < n; i++) work1[proclist[i]] += sizes[i];

  // size_send = # of bytes I send to each proc
  // size_self = # of bytes I copy to self
  // to balance pattern of send messages:
  //   each proc starts with iproc > me, continues until iproc = me

  int iproc = me;
  int isend = 0;
  for (i = 0; i < nprocs; i++) {
    iproc++;
    if (iproc == nprocs) iproc = 0;
    if (iproc == me) size_self = work1[iproc];
    else if (work1[iproc] > 0) size_send[isend++] = work1[iproc];
  }

  // tell receivers how many bytes I send them
  // sendmaxbytes = largest # of bytes I send in a single message

  sendmaxbytes = 0;
  for (i = 0; i < nsend; i++) {
    MPI_Send(&size_send[i],1,MPI_INT,proc_send[i],1,world);
    sendmaxbytes = MAX(sendmaxbytes,size_send[i]);
  }

  // receive incoming messages
  // num_recv = # of datums each proc sends me
  // nrecvdatum = total # of datums I recv

  int nbytes;
  bigint brecvsize = 0;
  for (i = 0; i < nrecv; i++) {
    MPI_Recv(&nbytes,1,MPI_INT,MPI_ANY_SOURCE,1,world,status);
    size_recv[proc2recv[status->MPI_SOURCE]] = nbytes;
    brecvsize += nbytes;
  }
  brecvsize += size_self;

  if (brecvsize > MAXSMALLINT) 
    error->one(FLERR,"Irregular comm recv buffer exceeds 2 GB");
  recvsize = brecvsize;

  // return # of datums I will receive

  return nrecvdatum;
}

/* ----------------------------------------------------------------------
   augment communication plan with new datums of uniform size
   called after create_procs() created initial plan
   n = # of datums to send
   proclist = proc to send each datum to, can include self
   return total # of datums I will recv
------------------------------------------------------------------------- */

int Irregular::augment_data_uniform(int n, int *proclist)
{
  int i,m,iproc,isend;

  // tally count of messages to each proc in num_send and num_self

  num_self = 0;
  for (i = 0; i < nsend; i++) work2[proc_send[i]] = 0;
  work2[me] = 0;
  for (i = 0; i < n; i++) work2[proclist[i]]++;
  for (i = 0; i < nsend; i++) num_send[i] = work2[proc_send[i]];
  num_self = work2[me];

  // reallocate send and self index lists if necessary
  // could use n-num_self for length of index_send to be more precise

  if (n > indexmax) {
    indexmax = n;
    memory->destroy(index_send);
    memory->create(index_send,indexmax,"irregular:index_send");
  }

  if (num_self > indexselfmax) {
    indexselfmax = num_self;
    memory->destroy(index_self);
    memory->create(index_self,indexselfmax,"irregular:index_self");
  }

  // work2 = offsets into index_send for each proc I send to
  // m = ptr into index_self
  // index_send = list of which datums to send to each proc
  //   1st N1 values are datum indices for 1st proc,
  //   next N2 values are datum indices for 2nd proc, etc
  // index_self = list of which datums to copy to self

  work2[0] = 0;
  for (i = 1; i < nsend; i++) work2[i] = work2[i-1] + num_send[i-1];

  if (num_self) {
    m = 0;
    for (i = 0; i < n; i++) {
      iproc = proclist[i];
      if (iproc == me) index_self[m++] = i;
      else {
        isend = work1[iproc];
        index_send[work2[isend]++] = i;
      }
    }
  } else {
    for (i = 0; i < n; i++) {
      isend = work1[proclist[i]];
      index_send[work2[isend]++] = i;
    }
  }

  // tell receivers how many datums I send them
  // sendmax = largest # of datums I send in a single message

  sendmax = 0;
  for (i = 0; i < nsend; i++) {
    MPI_Send(&num_send[i],1,MPI_INT,proc_send[i],0,world);
    sendmax = MAX(sendmax,num_send[i]);
  }

  // receive incoming messages
  // num_recv = # of datums each proc sends me
  // nrecvdatum = total # of datums I recv

  nrecvdatum = 0;
  for (i = 0; i < nrecv; i++) {
    MPI_Recv(&m,1,MPI_INT,MPI_ANY_SOURCE,0,world,status);
    iproc = status->MPI_SOURCE;
    num_recv[proc2recv[iproc]] = m;
    nrecvdatum += m;
  }
  nrecvdatum += num_self;

  // barrier to insure all MPI_ANY_SOURCE messages are received
  // else another proc could proceed to exchange_data() and send to me

  MPI_Barrier(world);

  // return # of datums I will receive

  return nrecvdatum;
}

/* ----------------------------------------------------------------------
   communicate uniform-size datums via existing plan
   sendbuf = list of datums to send
   nbytes = size of each datum
   recvbuf = received datums, including copied from me
------------------------------------------------------------------------- */

void Irregular::exchange_uniform(char *sendbuf, int nbytes, char *recvbuf)
{
  int i,m,n,offset,count;

  // post all receives, starting after self copies

  offset = num_self*nbytes;
  for (int irecv = 0; irecv < nrecv; irecv++) {
    MPI_Irecv(&recvbuf[offset],num_recv[irecv]*nbytes,MPI_CHAR,
	      proc_recv[irecv],0,world,&request[irecv]);
    offset += num_recv[irecv]*nbytes;
  }

  // reallocate buf for largest send if necessary

  if (sendmax*nbytes > bufmax) {
    memory->destroy(buf);
    bufmax = sendmax*nbytes;
    memory->create(buf,bufmax,"irregular:buf");
  }

  // send each message
  // pack buf with list of datums
  // m = index of datum in sendbuf

  n = 0;
  for (int isend = 0; isend < nsend; isend++) {
    count = num_send[isend];
    for (i = 0; i < count; i++) {
      m = index_send[n++];
      memcpy(&buf[i*nbytes],&sendbuf[m*nbytes],nbytes);
    }
    MPI_Send(buf,count*nbytes,MPI_CHAR,proc_send[isend],0,world);
  }       

  // copy datums to self, put at beginning of recvbuf

  for (i = 0; i < num_self; i++) {
    m = index_self[i];
    memcpy(&recvbuf[i*nbytes],&sendbuf[m*nbytes],nbytes);
  }

  // wait on all incoming messages

  if (nrecv) MPI_Waitall(nrecv,request,status);
}

/* ----------------------------------------------------------------------
   communicate variable-size datums via existing plan
   sendbuf = list of datums to send
   nbytes = size of each datum
   recvbuf = received datums, including copied from me
------------------------------------------------------------------------- */

void Irregular::exchange_variable(char *sendbuf, int *nbytes, char *recvbuf)
{
  int i,m,n,offset,count;

  // post all receives, starting after self copies

  offset = size_self;
  for (int irecv = 0; irecv < nrecv; irecv++) {
    if (update->ntimestep == 140 && me == 2)
      printf("POST RECV %p %d %d\n",this,proc_recv[irecv],size_recv[irecv]);

    MPI_Irecv(&recvbuf[offset],size_recv[irecv],MPI_CHAR,
	      proc_recv[irecv],0,world,&request[irecv]);
    offset += size_recv[irecv];
  }

  // reallocate buf for largest send if necessary

  if (sendmaxbytes > bufmax) {
    memory->destroy(buf);
    bufmax = sendmaxbytes;
    memory->create(buf,bufmax,"irregular:buf");
  }

  // send each message
  // pack buf with list of datums
  // m = index of datum in sendbuf
  // offset_send[m] = starting loc of datum in sendbuf

  n = 0;
  for (int isend = 0; isend < nsend; isend++) {
    offset = 0;
    count = num_send[isend];
    for (i = 0; i < count; i++) {
      m = index_send[n++];
      memcpy(&buf[offset],&sendbuf[offset_send[m]],nbytes[m]);
      offset += nbytes[m];
    }

    if (update->ntimestep == 140 && proc_send[isend] == 2)
      printf("SENDING from %d with %d bytes\n",me,size_send[isend]);
    if (update->ntimestep == 140 && me == 1 && proc_send[isend] == 2)
      printf("SENDING BUF %d\n",((Grid::ChildCell *) buf)->neigh[3]);


    MPI_Send(buf,size_send[isend],MPI_CHAR,proc_send[isend],0,world);
  }       

  // copy datums to self, put at beginning of recvbuf

  offset = 0;
  for (i = 0; i < num_self; i++) {
    m = index_self[i];
    memcpy(&recvbuf[offset],&sendbuf[offset_send[m]],nbytes[m]);
    offset += nbytes[m];
  }

  // wait on all incoming messages

  if (nrecv) MPI_Waitall(nrecv,request,status);

  if (update->ntimestep == 140 && me == 2) {
    printf("RECVING BUF %d %d %d\n",sizeof(cellint),num_self,
           ((Grid::ChildCell *) recvbuf)->neigh[3]);
  }

}
