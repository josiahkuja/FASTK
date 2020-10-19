/*********************************************************************************************\
 *
 *  Phase 2 of FastK:  For each set of the NTHREADS files for a each partition bucket produced
 *     by the split.c module, do the following:
 *       * Sort the super-mers.
 *       * Sort the k-mers from each super-mer weighted by the # of times that super-mer occurs.
 *       * During the 2nd sort accumulate the histogram of k-mer frequencies.
 *            Output this to <source_root>.K<kmer>.
 *       * if requesteda (-t) produce a table of all the k-mers with counts >= -t in NTHREADSs
 *            pieces in files SORT_PATH/<root>.<bucket>.L<thread>
 *       * if requested (-p) invert the first two sorts to produce a profile for every super-mer
 *            in the order in the source in files SORT_PATH/<root>.<bucket>.P<thread>.[0-3]
 *       * if requested (-h) print the histogram of k-mer frequencies.
 *
 *  Author:  Gene Myers
 *  Date  :  October, 2020
 *
 *********************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "gene_core.h"
#include "FastK.h"

#undef  DEBUG_COMPRESS
#undef  DEBUG_SLIST
#undef  DEBUG_KLIST
#undef  DEBUG_CANONICAL
#undef  DEBUG_TABOUT
#undef  DEBUG_CLIST
#undef  DEBUG_PLIST
#undef    SHOW_RUN
#undef  DEBUG_PWRITE

#define THREAD  pthread_t


/*******************************************************************************************
 *
 * static void *supermer_list_thread(Slist_Arg *arg)
 *     Each thread reads a file containing a part of the bit encoded super-mers for a given
 *     bucket and unpacks them into an array for sorting.  If -p each entry also contains
 *     it super-mer ordinal number in the input.
 *
 ********************************************************************************************/

  //  Unstuff ints and reads from a bit packed buffer

uint8 Comp[256];

#if defined(DEBUG_CANONICAL)||defined(DEBUG_KLIST)||defined(DEBUG_SLIST)||defined(DEBUG_TABOUT)

static char DNA[4] = { 'a', 'c', 'g', 't' };

static char *fmer[256], _fmer[1280];

#endif

#if defined(DEBUG_SLIST) || defined(DEBUG_TABOUT)

static void write_Ascii(uint8 *bytes, int len)
{ int i;

  for (i = 0; i < len; i += 4)
    printf("%s",fmer[*bytes++]);
}

#endif

static inline IO_UTYPE *Unstuff_Int(int64 *val, int nbits, IO_UTYPE mask, IO_UTYPE *buf, int *bitp)
{ int rem;

  rem = *bitp;
  if (nbits < rem)
    { rem -= nbits;
      *val = ((*buf) >> rem) & mask; 
#ifdef DEBUG_COMPRESS
      printf(" b = %2d/%016llx -%d-> v = %lld\n",rem,*buf,nbits,*val);
#endif
    }
  else if (nbits == rem)
    { *val = (*buf++) & mask;
      rem  = IO_UBITS;
#ifdef DEBUG_COMPRESS
      printf(" b = %2d/%016llx -%d-> v = %lld\n",rem,buf[-1],nbits,*val);
#endif
    }
  else
    { IO_UTYPE x = (*buf++) << (nbits-rem);
      rem += IO_UBITS - nbits;
      *val = (x | (*buf >> rem)) & mask;
#ifdef DEBUG_COMPRESS
      printf(" b = %2d/%016llx|%016llx -%d-> v = %lld\n",rem,buf[-1],*buf,nbits,*val);
#endif
    }
  *bitp = rem;
  return (buf);
}

static inline IO_UTYPE *Unstuff_Code(uint8 *s, int len, IO_UTYPE *buf, int *bitp)
{ int      i, rem;
  IO_UTYPE x;

  rem = *bitp;
  len <<= 1;
  for (i = 8; i < len; i += 8)
    if (rem > 8)
      { rem -= 8;
        *s++ = ((*buf) >> rem) & 0xff;
      }
    else if (rem == 8)
      { *s++ = *buf++ & 0xff;
        rem  = IO_UBITS;
      }
    else
      { x = (*buf++) << (8-rem);
        rem += IO_UBITS - 8;
        *s++ = (x | (*buf >> rem)) & 0xff;
      }
  i = len - (i-8); 
  if (i > 0)
    { if (rem > i)
        { rem -= i;
          x = ((*buf) >> rem);
        }
      else if (rem == i)
        { x = *buf++;
          rem  = IO_UBITS;
        }
      else
        { x = (*buf++) << (i-rem);
          rem += IO_UBITS - i;
          x |= ((*buf) >> rem);
        }
      *s++ = (x << (8-i)) & 0xff;
    }
  *bitp = rem;
  return (buf);
}

  // Read smers stuffed super-mers from tfile producing kmer list, loading at fill

static int  Fixed_Reload[IO_UBITS+1];
static int  Runer_Reload[IO_UBITS+1];
static int *Super_Reload[IO_UBITS+1];

typedef struct
  { int     tfile;      //  Bit compressed super-mer input streaam for thread
    int64   nmers;      //  # of super-mers in the input
    int64   nbase;      //  if DO_PROFILE then start of indices for this thread
    uint8  *fours[256]; //  finger for filling list sorted on first super-mer byte
  } Slist_Arg;

static void *supermer_list_thread(void *arg)
{ Slist_Arg   *data = (Slist_Arg *) arg;
  int64        nmers = data->nmers;
  int64        nbase = data->nbase;
  int          in    = data->tfile;
  uint8      **fours = data->fours;

  uint8 *fill, *prev;
  int64  f, *fb = &f;
  int64  rmask, rlim;
  int    N, i, rbits;

  int64 r = 0;
  int64 n = 0;
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
  uint8  *nb = ((uint8 *) &n) - 1;
  uint8  *rb = ((uint8 *) &r) - 1;
#else
  uint8  *nb = ((uint8 *) &n) + (sizeof(int64)-SLEN_BYTES);
  uint8  *rb = ((uint8 *) &r) + (sizeof(int64)-RUN_BYTES);
#endif

  IO_UTYPE iobuf[IO_BUF_LEN], *ioend, *ptr;
  int    bit, clen; 
  int64  k;

  read(in,iobuf,IO_UBYTES*IO_BUF_LEN);
  ioend = iobuf + IO_BUF_LEN;
  ptr   = iobuf;
  bit   = IO_UBITS;
  rbits = 17;
  rlim  = 0x10000ll;
  rmask = 0x1ffffll;

#ifdef DEBUG_SLIST
  printf("Index at %d(%llx)\n",rbits,rmask);
#endif

  for (k = 0; k < nmers; k++)
    { while (1)
        { if (ptr + Fixed_Reload[bit] >= ioend)
            { int res = 0;
              while (ptr < ioend)
                iobuf[res++] = *ptr++;
              read(in,iobuf+res,IO_UBYTES*(IO_BUF_LEN-res));
              ptr = iobuf;
            }
          ptr = Unstuff_Int(&n,SLEN_BITS,SLEN_BIT_MASK,ptr,&bit);

          if (n < MAX_SUPER)
            break;

          *prev |= 0x80;
#ifdef DEBUG_SLIST
          printf("+++\n");
#endif
        }

      if (ptr + Super_Reload[bit][n] >= ioend)
        { int res = 0;
          while (ptr < ioend)
            iobuf[res++] = *ptr++;
          read(in,iobuf+res,IO_UBYTES*(IO_BUF_LEN-res));
          ptr = iobuf;
        }

      N = n+KMER;

      ptr = Unstuff_Int(fb,8,0xffllu,ptr,&bit);
      
      fill = fours[f];
      fours[f] += SMER_WORD;
      *fill++ = 0;

      ptr = Unstuff_Code(fill,N-4,ptr,&bit);
      clen = ((N-1)>>2);
      fill += clen;

      for (i = clen+1; i < SMER_BYTES; i++)
        *fill++ = 0;
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
      for (i = SLEN_BYTES; i > 0; i--)
        *fill++ = nb[i];
#else
      for (i = 0; i < SLEN_BYTES; i++)
        *fill++ = nb[i];
#endif

      if (DO_PROFILE)
        { while (1)
            { if (ptr + Runer_Reload[bit] >= ioend)
                { int res = 0;
                  while (ptr < ioend)
                    iobuf[res++] = *ptr++;
                  read(in,iobuf+res,IO_UBYTES*(IO_BUF_LEN-res));
                  ptr = iobuf;
                }
              ptr = Unstuff_Int(&r,rbits,rmask,ptr,&bit);
              if (r < rlim)
                 break;
              rbits  += 1;
              rlim  <<= 1;
              rmask   = (rmask << 1) + 1;
#ifdef DEBUG_SLIST
              printf("Index bumped to %d(%llx) r = %llu\n",rbits,rmask,r);
#endif
            }
          r += nbase;
          prev = fill;
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
          for (i = RUN_BYTES; i > 0; i--)
            *fill++ = rb[i];
#else
          for (i = 0; i < RUN_BYTES; i++)
            *fill++ = rb[i];
#endif
        }

#ifdef DEBUG_SLIST
      if (DO_PROFILE)
        printf(" %5lld: ",r);
      printf("[%02llx] %s ",f,fmer[f]);
      write_Ascii(fill-(SMER_WORD-1),N-4);
      printf(" : %d\n",N);
      printf("            ");
      for (i = -SMER_WORD; i < 0; i++)
        printf(" %02x",fill[i]);
      printf("\n");
#endif
    }

  if (ptr + Fixed_Reload[bit] >= ioend)
    { int res = 0;
      while (ptr < ioend)
        iobuf[res++] = *ptr++;
      read(in,iobuf+res,IO_UBYTES*(IO_BUF_LEN-res));
      ptr = iobuf;
    }
  ptr = Unstuff_Int(&n,SLEN_BITS,SLEN_BIT_MASK,ptr,&bit);
  if (n >= MAX_SUPER)
    { *prev |= 0x80;
#ifdef DEBUG_SLIST
      printf("+++\n");
#endif
    }

  return (NULL);
}


/*******************************************************************************************
 *
 * static void *kmer_list_thread(Klist_Arg *arg)
 *     Each thread takes the now sorted super-mers, counts the # of each unique super-mer,
 *     and places the canonical k-mers of each with weights in an array for sorting.
 *     If the -p option is set then each k-mer entry also has the ordinal number of the
 *     k-mer in order of generation.
 *
 ********************************************************************************************/

typedef struct
  { uint8    *sort;
    int64    *parts;
    int       beg;
    int       end;
    int64     off;
    uint8    *fours[256];
    int64     kidx;
  } Klist_Arg;

static int kclip[4] = { 0xff, 0xc0, 0xf0, 0xfc };

static void *kmer_list_thread(void *arg)
{ Klist_Arg   *data   = (Klist_Arg *) arg;
  int          beg    = data->beg;
  int          end    = data->end;
  int64       *part   = data->parts;
  uint8      **fours  = data->fours;

  int       KMp3   = KMER+3;
  int       KRS    = ((KMER-1)&0x3)<<1;
  int       KMd2   = (KMER_BYTES+1)>>1;
  int       fptr[SMER_BYTES+1], rptr[SMER_BYTES+1];
  int      *FPT    = fptr-1;
  int      *RPT    = rptr-(KMER_BYTES-1);
  int       KCLIP  = kclip[KMER&0x3];

  uint8    *sptr, *send, *lptr;
  int       x, ct, sbytes;
  uint8    *asp, *fill;

int show;
  int64  ridx = 0;
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
  uint8  *rbx = ((uint8 *) &ridx) - 1;
#else
  uint8  *rbx = ((uint8 *) &ridx) + (sizeof(int64)-RUN_BYTES);
#endif
#ifdef DEBUG_KLIST
#endif

  int       i, o;
  int      *f, *r;
  int       fb, rb;
  int       fs, rs;
  int       kb, hb;
  int       kf, hf;

  int   sln = 0;
  int64 idx = 0;
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
  uint8  *sb = ((uint8 *) &sln) - 1;
  uint8  *ib = ((uint8 *) &idx) - 1;
#else
  uint8  *sb = ((uint8 *) &sln) + (sizeof(int)-SLEN_BYTES);
  uint8  *ib = ((uint8 *) &idx) + (sizeof(int64)-KMAX_BYTES);
#endif

  idx  = data->kidx;
  sptr = data->sort + data->off;
  for (x = beg; x < end; x++)
    for (send = sptr + part[x]; sptr < send; sptr = lptr)

      { asp = sptr + SMER_BYTES;
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
        for (i = SLEN_BYTES; i > 0; i--)
          sb[i] = *asp++;
#else
        for (i = 0; i < SLEN_BYTES; i++)
          sb[i] = *asp++;
#endif
 
show = 0;
        // printf("Run ids:");
        lptr = sptr;
        do
          { lptr += SMER_WORD;
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
            for (i = RUN_BYTES; i > 0; i--)
              rbx[i] = lptr[-i];
            rbx[RUN_BYTES] &= 0x7ff;
#else
            for (i = 0; i < RUN_BYTES; i++)
              rbx[i] = lptr[i-RUN_BYTES];
            rbx[0] &= 0x7ff;
#endif
if (ridx == 0)
  { printf("Ridx 0 kmers began at %lld\n",idx);
    show = 1;
  }
            // printf(" %c%lld",lptr[KMER_WORD]&0x8?'+':' ',ridx);
          }
        while (*lptr == 0);
#ifdef DEBUG_KLIST
        printf("\n");
#endif

        lptr = sptr + SMER_WORD;
        ct   = 1;
        while (*lptr == 0)
          { lptr += SMER_WORD;
            ct   += 1;
          }
if (show)
  printf("Super-mer is %d k-mers\n",ct);

        *sptr  = x;
        sbytes = (KMp3 + sln) >> 2;

#ifdef DEBUG_KLIST
        printf("%02x/%lld: l=%d c=%d\n  ",x,idx,sln+1,ct);
        for (i = 0; i < sbytes; i++)
          printf(" %s",fmer[sptr[i]]);
        printf("\n");
        fflush(stdout);
#endif

        for (o = sbytes, i = 0; i <= sbytes; i++, o--)
          { fptr[i] = (sptr[i] << 8) | sptr[i+1];
            rptr[i] = (Comp[sptr[o]] << 8) | Comp[sptr[o-1]];
          }

#ifdef DEBUG_CANONICAL
        printf("   F = ");
        for (i = 0; i < sbytes; i++)
          printf(" %s%s",fmer[fptr[i]>>8],fmer[fptr[i]&0xff]);
        printf("\n   R = ");
        for (i = 0; i < sbytes; i++)
          printf(" %s%s",fmer[rptr[i]>>8],fmer[rptr[i]&0xff]);
        printf("\n");
        fflush(stdout);
#endif

        f  = FPT;
        fs = 2;
        r  = RPT + sbytes;
        rs = KRS;
        rb = *r;
        for (o = 0; o <= sln; o++)
          { fs -= 2;
            rs += 2;
            if (fs == 0)
              { fb = *++f;
                fs = 8;
              }
            kb = kf = (fb >> fs) & 0xff;
            if (rs == 8)
              { rb = *--r;
                rs = 0;
              }
            hb = hf = (rb >> rs) & 0xff;
#ifdef DEBUG_CANONICAL
            printf("   + %d / %s%s / %s\n",fs, fmer[fb>>8], fmer[fb&0xff], fmer[kb]);
            printf("   - %d / %s%s / %s\n",rs, fmer[rb>>8], fmer[rb&0xff], fmer[hb]);
            fflush(stdout);
#endif
            if (kb == hb)
              { for (i = 1; i < KMd2; i++)
                  { kb = (f[i] >> fs) & 0xff;
                    hb = (r[i] >> rs) & 0xff;
#ifdef DEBUG_CANONICAL
                    printf("      @ %d: %s vs %s\n",i,fmer[kb],fmer[hb]);
                    fflush(stdout);
#endif
                    if (kb != hb)
                      break;
                  }
              }

            if (kb < hb)
              { fill = fours[kf];
                fours[kf] = fill+KMER_WORD;
                *fill++ = 0;
                for (i = 1; i < KMER_BYTES; i++)
                  *fill++ = (f[i] >> fs) & 0xff;
              }
            else
              { fill = fours[hf];
                fours[hf] = fill+KMER_WORD;
                *fill++ = 0;
                for (i = 1; i < KMER_BYTES; i++)
                  *fill++ = (r[i] >> rs) & 0xff;
              }
            fill[-1] &= KCLIP;
            *((uint16 *) fill) = ct;
            fill += 2;
            if (DO_PROFILE)
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
              { for (i = KMAX_BYTES; i > 0; i--)
                  *fill++ = ib[i];
#else
              { for (i = 0; i < KMAX_BYTES; i++)
                  *fill++ = ib[i];
#endif
                idx  += 1;
              }

#ifdef DEBUG_KLIST
            printf("    [%02x/%s]:",kf < hf ? kf : hf,fmer[kf < hf ? kf : hf]);
            for (i = 1-KMER_WORD; i < KMER_BYTES-KMER_WORD; i++)
              printf(" %s",fmer[fill[i]]);
            for (i = KMER_BYTES-KMER_WORD; i < 0; i++)
              printf(" %02x",fill[i]);
            printf("\n");
            fflush(stdout);
#endif
          }

        *sptr = 1;
      }

  return (NULL);
}


/*******************************************************************************************
 *
 * static void *table_write_thread(Twrite_Arg *arg)
 *     Each thread takes the now sorted weighted k-mers, adds together the wgt.s of each
 *     unique k-mer, and outputs to a "L" file each k-mer and count pair that has weight
 *     greater thaan DO_TABLE (the -t option value, called only if set).
 *
 ********************************************************************************************/
typedef struct
  { uint8    *sort;
    int64    *parts;
    int       beg;
    int       end;
    int64     off;
    int       kfile;        // used by table_write_threaad
  } Twrite_Arg;

static void *table_write_thread(void *arg)
{ Twrite_Arg  *data   = (Twrite_Arg *) arg;
  int          beg    = data->beg;
  int          end    = data->end;
  int64       *part   = data->parts;
  int          kfile  = data->kfile;

  int    TMER_WORD = KMER_BYTES+2;

  uint8  bufr[0x10000];
  uint8 *fill, *bend;
  int    x, ct;
  uint8 *kptr, *lptr, *kend;

  fill = bufr;
  bend = bufr + (0x10000 - TMER_WORD);

  kptr = data->sort + data->off;
  for (x = beg; x < end; x++)
    for (kend = kptr + part[x]; kptr < kend; kptr = lptr)
      { ct = *((uint16 *) (kptr+KMER_BYTES));
        lptr = kptr+KMER_WORD;
        while (*lptr == 0)
          lptr += KMER_WORD;
        if (ct >= DO_TABLE)
          { if (fill >= bend)
              { write(kfile,bufr,fill-bufr);
                fill = bufr;
              }

            if (ct >= 0x8000)
              *((uint16 *) (kptr+KMER_BYTES)) = 0x8000;
            *kptr = x;
            memcpy(fill,kptr,TMER_WORD);
#ifdef DEBUG_TABOUT
            write_Ascii(kptr,KMER);
            printf(" %hd\n",*((uint16 *) (kptr+KMER_BYTES)));
#endif
            fill += TMER_WORD;
          }
      }
  write(kfile,bufr,fill-bufr);
  close(kfile);

  return (NULL);
}


/*******************************************************************************************
 *
 * static void *cmer_list_thread(Clist_Arg *arg)
 *     Each thread takes the now sorted weighted k-mers entries and reduces each to
 *     an entry containg the count and k-mer ordinal index, in preparation for the
 *     first of two "inverting" sorts to realize the -p option.
 *
 ********************************************************************************************/

typedef struct
  { uint8    *sort;
    int64    *parts;
    int       beg;
    int       end;
    int64     off;
    uint8    *fours[256];
  } Clist_Arg;

static void *cmer_list_thread(void *arg)
{ Clist_Arg   *data   = (Clist_Arg *) arg;
  int          beg    = data->beg;
  int          end    = data->end;
  int64       *part   = data->parts;
  uint8      **fours  = data->fours;

  int KM1 = KMER_WORD-1;

  int    x, d, k;
  uint8 *kptr, *kend;
  uint8 *fill;

  kptr = data->sort + data->off;
  for (x = beg; x < end; x++)
    for (kend = kptr + part[x]; kptr < kend; kptr += KMER_WORD)
      { d = kptr[KM1];
        fill = fours[d];
        for (k = KMER_BYTES; k < KM1; k++)
          *fill++ = kptr[k];
        fours[d] = fill;
      } 

  return (NULL);
}


/*******************************************************************************************
 *
 * static void *profile_list_thread(Plist_Arg *arg)
 *     Each thread takes the now "un"sorted weighted k-mers entries that are now in order
 *     of the k-mers along each super-mer.  Produce a profile for each unique super-mer
 *     in place and build an array of entries consisting of a pointer to the profile fragment
 *     and the ordinal super-mer id of each of the equall super-mers with that profile.
 *     This array will be "un"sorted on super-mer id.
 *
 ********************************************************************************************/

typedef struct
  { uint8    *sort;
    int64    *parts;
    int       beg;
    int       end;
    int64     off;
    uint8    *prol;
    int64     cnts;
    uint8    *fill;  //  profile list uses this (basaed on the counts)
  } Plist_Arg;

static void *profile_list_thread(void *arg)
{ Plist_Arg   *data   = (Plist_Arg *) arg;
  int          beg    = data->beg;
  int          end    = data->end;
  int64       *part   = data->parts;

  uint8    *cnts, *fill, *prof;
  uint8    *sptr, *send;
  int       i, x;
  uint8    *asp;
  uint8     b[2*MAX_SUPER];
  uint64    pidx;

  int       STOT = SMER_BYTES + SLEN_BYTES;

  int sln = 0;
  int len = 0;
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
  uint8  *sb = ((uint8 *) &sln) - 1;
  uint8  *lb = ((uint8 *) &len) - 1;
#else
  uint8  *sb = ((uint8 *) &sln) + (sizeof(int)-SLEN_BYTES);
  uint8  *lb = ((uint8 *) &len) + (sizeof(int)-PLEN_BYTES);
#endif

  int64  ridx = 0;
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
  uint8  *rbx = ((uint8 *) &ridx) - 1;
#else
  uint8  *rbx = ((uint8 *) &ridx) + (sizeof(int64)-RUN_BYTES);
#endif
int show;
#ifdef SHOW_RUN
#endif

  prof = data->prol;
  pidx = CMER_WORD*data->cnts;
  cnts = data->prol + pidx;
  sptr = data->sort + data->off;
  fill = data->fill;
  for (x = beg; x < end; x++)
    for (send = sptr + part[x]; sptr < send; )

      { asp = sptr + SMER_BYTES;
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
        for (i = SLEN_BYTES; i > 0; i--)
          sb[i] = *asp++;
#else
        for (i = 0; i < SLEN_BYTES; i++)
          sb[i] = *asp++;
#endif

show = ((int64) (asp-data->sort) == 0x1d4a4aecll);

        { int j, p, c, d;
          int run;
          uint8 *db = (uint8 *) &d;

          p = *((uint16 *) cnts);
          cnts += CMER_WORD;

          *((uint16 *) b) = p;
          len = 2;
if (show)
          printf("  {%d}",p);
#ifdef SHOW_RUN
#endif

          run = 0;
          for (j = 1; j <= sln; j++)
            { c = *((uint16 *) cnts);
              cnts += CMER_WORD;

              if (c == p)
                if (run > 0)
                  { if (run >= 63)
                      { b[len++] = run;
#ifdef SHOW_RUN
                        printf(" [%d]",run);
#endif
                        run = 1;
                      }
                    else
                      run += 1;
                  }
                else
                  run = 1;
              else
                { if (run > 0)
                    { b[len++] = run;
#ifdef SHOW_RUN
                      printf(" [%d]",run);
#endif
                      run = 0;
                    }
                  d = c-p;
#ifdef SHOW_RUN
                  printf(" %d",d);
#endif
                  if (abs(d) < 32)
                    b[len++] = 0x40 | (d & 0x3f);
                  else
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
                    { b[len++] = db[1] | 0x80;
                      b[len++] = db[0];
#else
                    { b[len++] = db[0] | 0x80;
                      b[len++] = db[1];
#endif
#ifdef SHOW_RUN
                      printf("+");
#endif
                    }
                }

              p = c;
            }

          if (run > 0)
            { b[len++] = run;
#ifdef SHOW_RUN
              printf(" [%d]",run);
#endif
            }
          if (len > 2)
            { *((uint16 *) (b+len)) = p;
#ifdef SHOW_RUN
              printf(" {%d}",p);
#endif
            }
#ifdef SHOW_RUN
          printf(" += %d / %d\n",len,sln+1);
#endif
        }

#ifdef SHOW_RUN
        printf("Run ids:");
#endif

        do
          { if (sptr[STOT] & 0x80)
              { *((uint64 *) fill) = (pidx<<1) | 0x1;
                sptr[STOT] &= 0x7f;
#ifdef SHOW_RUN
                printf(" +");
#endif
              }
            else
              { *((uint64 *) fill) = (pidx<<1);
#ifdef SHOW_RUN
                printf("  ");
#endif
              }
            fill += sizeof(uint64);
            for (i = STOT; i < SMER_WORD; i++)
              *fill++ = sptr[i];

#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
            for (i = RUN_BYTES; i > 0; i--)
              rbx[i] = sptr[SMER_WORD-i];
#else
            for (i = 0; i < PLEN_BYTES; i++)
              rbx[i] = sptr[STOT+i];
#endif
if (ridx == 0)
  printf("Frag 0 has asp = %ld\n",asp-data->sort);
#ifdef SHOW_RUN
            // printf("%lld",ridx);
#endif

            sptr += SMER_WORD;
          }
        while (*sptr == 0);

#ifdef SHOW_RUN
        printf(" <%ld>\n",(sptr-asp)/SMER_WORD+1);
#endif

#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
        for (i = PLEN_BYTES; i > 0; i--)
          prof[pidx++] = lb[i];
#else
        for (i = 0; i < PLEN_BYTES; i++)
          prof[pidx++] = lb[i];
#endif
        if (len > 2)
          len += 2;
        for (i = 0; i < len; i++)
          prof[pidx++] = b[i];
      }

  return (NULL);
}


/*******************************************************************************************
 *
 * static void *profile_write_thread(Pwrite_Arg *arg)
 *     Each thread takes the now "un"sorted list of pointers to profile fragments and
 *     outputs them to a "P" file in the SORT_PATH directory.
 *
 ********************************************************************************************/

typedef struct
  { uint8    *sort;
    char     *root;
    int       wch;
    int64     beg;
    int64     end;
    uint8    *prol;
  } Pwrite_Arg;

static void *profile_write_thread(void *arg)
{ Pwrite_Arg  *data   = (Pwrite_Arg *) arg;
  uint8       *prol   = data->prol;
  int64        beg    = data->beg;
  int64        rng    = data->end - beg;

  int    pfile;
  char  *fname;
  uint8 *psort, *next;
  uint8 *prof;
  uint8  bufr[0x10000];
  uint8 *fill, *bend;
  uint64 pidx;
  int    last;
  int    t, k;

  int len = 0;
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
  uint8  *lb = ((uint8 *) &len) - 1;
#else
  uint8  *lb = ((uint8 *) &len) + (sizeof(int)-PLEN_BYTES);
#endif

#ifdef DEBUG_PWRITE
  int64   r = 0;
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
  uint8  *rb = ((uint8 *) &r) - 1;
#else
  uint8  *rb = ((uint8 *) &r) + (sizeof(int64)-RUN_BYTES);
#endif
#endif

  fname = Malloc(strlen(data->root) + 100,"File Name");
  bend  = bufr + (0x10000 - (RUN_BYTES+PLEN_BYTES+2*MAX_SUPER));

  psort = data->sort + beg*PROF_BYTES;
  for (t = 1; t <= NPANELS; t++)
    { next  = data->sort + (beg + (rng*t)/NPANELS) * PROF_BYTES;

#ifdef DEBUG_PWRITE
      printf("Panel %d\n",t);
#endif
      sprintf(fname,"%s%d.%d",data->root,data->wch,t-1);
      pfile = open(fname,O_WRONLY|O_CREAT|O_TRUNC,S_IRWXU|S_IRWXG|S_IRWXO);
      if (pfile < 0)
        { fprintf(stderr,"\n\n%s: Could not open %s for writing\n",Prog_Name,fname);
          exit (1);
        }

#ifdef DEVELOPER
      if (t == 1 && data->wch == 0)
        { write(pfile,&RUN_BYTES,sizeof(int));
          write(pfile,&PLEN_BYTES,sizeof(int));
          write(pfile,&MAX_SUPER,sizeof(int));
        }
#endif

      fill = bufr;
      while (psort < next)
        { if (fill >= bend)
            { write(pfile,bufr,fill-bufr);
              fill = bufr;
            }

          pidx = *((uint64 *) psort);
          psort += sizeof(uint64);

          last = (pidx & 0x1);
          pidx >>= 1;

#ifdef DEBUG_PWRITE
#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
          for (k = RUN_BYTES; k > 0; k--)
            rb[k] = psort[RUN_BYTES-k];
#else
          for (k = 0; k < RUN_BYTES; k++)
            rb[k] = psort[k];
#endif
#endif

          if (last)
            *fill++ = *psort++ | 0x80;
          else
            *fill++ = *psort++;
          for (k = 1; k < RUN_BYTES; k++)
            *fill++ = *psort++;

          prof = prol + pidx;

#if __ORDER_LITTLE_ENDIAN__ == __BYTE_ORDER__
          for (k = PLEN_BYTES; k > 0; k--)
            lb[k] = *fill++ = *prof++;
#else
          for (k = 0; k < PLEN_BYTES; k++)
            lb[k] = *fill++ = *prof++;
#endif

#ifdef DEBUG_PWRITE
          { uint16 x, d;

            printf("%8ld: %8lld%c (%9lld), %3d:",
                   (psort-data->sort)/PROF_BYTES,r,last?'+':' ',pidx,len);

            x = *((uint16 *) prof);
            k = 2;
            printf(" {%d}",x);
            while (k < len)
              { x = prof[k++];
                if ((x & 0x80) != 0)
                  { if ((x & 0x40) != 0)
                      d = (x << 8);
                    else
                      d = (x << 8) & 0x7fff;
                    x = prof[k++];
                    d |= x;
                    printf(" %hd+",d);
                  }
                else if ((x & 0x40) != 0)
                  { if ((x & 0x20) != 0)
                      printf(" -%d",32-(x&0x1fu));
                    else
                      printf(" +%d",x&0x1fu);
                  }
                else
                  printf(" [%hu]",x);
              }
            if (len > 2)
              printf(" {%d}",*((uint16 *) (prof+len)));
            printf("\n");
          }
#endif

          if (len > 2)
            len += 2;
          for (k = 0; k < len; k++)
            *fill++ = *prof++;
        }
      write(pfile,bufr,fill-bufr);
      close(pfile);
    }

  return (NULL);
}


/*********************************************************************************************\
 *
 *  Sorting(char *dpwd, char *dbrt)
 *
 *     For each set of the NTHREADS files for a each partition bucket produced
 *     by the split.c module, do the following:
 *       * Sort the super-mers.
 *       * Sort the k-mers from each super-mer weighted by the # of times that super-mer occurs.
 *       * During the 2nd sort accumulate the histogram of k-mer frequencies.
 *            Output this to <source_root>.K<kmer>.
 *       * if requesteda (-t) produce a table of all the k-mers with counts >= -t in NTHREADSs
 *            pieces in files SORT_PATH/<root>.<bucket>.L<thread>
 *       * if requested (-p) invert the first two sorts to produce a profile for every super-mer
 *            in the order in the source in files SORT_PATH/<root>.<bucket>.P<thread>.[0-3]
 *       * if requested (-h) print the histogram of k-mer frequencies.
 *
 *********************************************************************************************/

void Sorting(char *dpwd, char *dbrt)
{ char  *fname;
  int64  counts[0x8000];
  int   *reload;

  if (VERBOSE)
    { fprintf(stderr,"\nPhase 2: Sorting & Counting K-mers in %d blocks\n\n",NPARTS);
      fflush(stderr);
    }

  fname = Malloc(strlen(SORT_PATH) + strlen(dpwd) + strlen(dbrt) + 100,"File name buffer");
  if (fname == NULL)
    exit (1);

  //  Remove any previous results for this DB in this directory with this KMER value

  sprintf(fname,"rm -f %s/%s.K%d",dpwd,dbrt,KMER);
  system(fname);

  //  First bundle: initialize all sizes & lookup tables

  { int  i, n;
    int  l0, l1, l2, l3;
    int *s;

    reload  = Malloc(sizeof(int)*IO_UBITS*MAX_SUPER,"Allocating reload table");

    for (i = 0; i < 0x8000; i++)
      counts[i] = 0;

    s = reload;
    for (i = 0; i < IO_UBITS; i++)
      { Fixed_Reload[IO_UBITS-i] = (i + SLEN_BITS - 1) / IO_UBITS;
        Runer_Reload[IO_UBITS-i] = (i + RUN_BITS - 1) / IO_UBITS;
        Super_Reload[IO_UBITS-i] = s;
        for (n = 0; n < MAX_SUPER; n++)
          *s++ = (i + 2*(n+KMER) - 1) / IO_UBITS;
      }

    i = 0;
    for (l0 = 3; l0 >= 0; l0 -= 1)
     for (l1 = 12; l1 >= 0; l1 -= 4)
      for (l2 = 48; l2 >= 0; l2 -= 16)
       for (l3 = 192; l3 >= 0; l3 -= 64)
         Comp[i++] = (l3 | l2 | l1 | l0);

#if defined(DEBUG_CANONICAL)||defined(DEBUG_KLIST)||defined(DEBUG_SLIST)||defined(DEBUG_TABOUT)
    { char *t;

      i = 0;
      t = _fmer;
      for (l3 = 0; l3 < 4; l3++)
       for (l2 = 0; l2 < 4; l2++)
        for (l1 = 0; l1 < 4; l1++)
         for (l0 = 0; l0 < 4; l0++)
           { fmer[i] = t;
             *t++ = DNA[l3];
             *t++ = DNA[l2];
             *t++ = DNA[l1];
             *t++ = DNA[l0];
             *t++ = 0;
             i += 1;
           }
    }
#endif
  }

  // For each partition, make k-mer list, sort on k-mer and count, resort on position,
  //    and finally output super-counts

  { Slist_Arg   parms[NTHREADS];
    Klist_Arg   parmk[NTHREADS];
    Clist_Arg   parmc[NTHREADS];
    Twrite_Arg  parmt[NTHREADS];
    Plist_Arg   parmp[NTHREADS];
    Pwrite_Arg  parmw[NTHREADS];

    int         Table_Split[NTHREADS];
    int64       Sparts[256];
    int64       Kparts[256];
    Range       Panels[NTHREADS];
    int64       Wkmers[NPARTS];
    int64       Ukmers[NPARTS];

    uint8      *s_sort;
    uint8      *k_sort;
    uint8      *i_sort;
    uint8      *p_sort;
    uint8      *a_sort;

    int         ODD_PASS;

#if !defined(DEBUG) || !defined(SHOW_RUN)
    THREAD      threads[NTHREADS];
#endif

    int64  kmers;
    int64  nmers;
    int64  skmers;
    int    t, p;

    s_sort = NULL;
    i_sort = NULL;

#ifndef DEVELOPER
    if (DO_PROFILE)
      { KMER_WORD += KMAX_BYTES;
        CMER_WORD  = KMAX_BYTES+1;
        ODD_PASS   = (KMAX_BYTES % 2 == 1);
        RUN_BYTES  = (RUN_BITS+7) >> 3;
        SMER_WORD += RUN_BYTES;
        PROF_BYTES = RUN_BYTES + sizeof(uint64);
      }
    s_sort = Malloc((NMAX+1)*SMER_WORD,"Allocating super-mer sort array");
    if (s_sort == NULL)
      exit (1);
#endif

    for (p = 0; p < NPARTS; p++)
      {
        //  Open and reada headers of super-mer files for part p

        kmers = 0;
        nmers = 0;
        for (t = 0; t < NTHREADS; t++)
          { int64 k, n;
            int   f;

            sprintf(fname,"%s/%s.%d.T%d",SORT_PATH,dbrt,p,t);
            f = open(fname,O_RDONLY);
            if (f < 0)
              { fprintf(stderr,"\n\n%s: File %s should exist but doesn't?\n",Prog_Name,fname); 
                exit (1);
              }

            parms[t].tfile = f;
#ifdef DEVELOPER
            read(f,&KMAX,sizeof(int64));
            read(f,&NMAX,sizeof(int64));
            read(f,&KMAX_BYTES,sizeof(int));
            read(f,&RUN_BITS,sizeof(int));
#endif
            read(f,&k,sizeof(int64));
            read(f,&n,sizeof(int64));
            parms[t].nmers = n;
            kmers += k;
            nmers += n;
            read(f,&n,sizeof(int64));
            parms[t].nbase = n;

            read(f,Panels[t].khist,sizeof(int64)*256);
          }

#ifdef DEVELOPER
        if (p == 0)
          { if (DO_PROFILE)
              { KMER_WORD += KMAX_BYTES;
                CMER_WORD  = KMAX_BYTES+1;
                ODD_PASS   = (KMAX_BYTES % 2 == 1);
                RUN_BYTES  = (RUN_BITS+7) >> 3;
                SMER_WORD += RUN_BYTES;
                PROF_BYTES = RUN_BYTES + sizeof(uint64);
                for (int i = 0; i < IO_UBITS; i++)
                  Runer_Reload[IO_UBITS-i] = (i + RUN_BITS - 1) / IO_UBITS;
              }
            s_sort = Malloc((NMAX+1)*SMER_WORD,"Allocating super-mer sort array");
            if (s_sort == NULL)
              exit (1);
          }
#endif

        //  Build super-mer list

        if (VERBOSE)
          { fprintf(stderr,"\r  Processing part %d: Sorting super-mers     ",p); 
            fflush(stderr);
          }

        { int64 o, x;
          int   j;

          o = 0;
          for (j = 0; j < 256; j++)
            for (t = 0; t < NTHREADS; t++)
              { parms[t].fours[j] = s_sort + o*SMER_WORD;
                o += Panels[t].khist[j];
              }
          if (o != nmers)
            { fprintf(stderr,"o != nmers (%lld vs %lld)\n",o,nmers);
              exit (1);
            }

          o = 0;
          for (t = 0; t < NTHREADS; t++)
            { x = parms[t].nbase;
              parms[t].nbase = o;
              o += x;
            }
        }

#ifdef DEBUG_SLIST
        for (t = 0; t < NTHREADS; t++)
          supermer_list_thread(parms+t);
#else
        for (t = 1; t < NTHREADS; t++)
          pthread_create(threads+t,NULL,supermer_list_thread,parms+t);
	supermer_list_thread(parms);
        for (t = 1; t < NTHREADS; t++)
          pthread_join(threads[t],NULL);
#endif

        for (t = 0; t < NTHREADS; t++)
          close(parms[t].tfile);

#ifndef DEVELOPER
        for (t = 0; t < NTHREADS; t++)
          { sprintf(fname,"%s/%s.%d.T%d",SORT_PATH,dbrt,p,t);
            unlink(fname);
          }
#endif

        //  Sort super-mer list

        { uint8 *o, *x;
          int    j;

          o = s_sort;
          for (j = 0; j < 256; j++)
            { x = parms[NTHREADS-1].fours[j];
              Sparts[j] = x-o;
              o = x;
            }
        }

        Supermer_Sort(s_sort,nmers,SMER_WORD,SMER_BYTES+SLEN_BYTES,Sparts,NTHREADS,Panels);

        //  Allocate and fill in weighted k-mer list from sorted supermer list

        { int64 o, x;
          int   j;

          if (DO_PROFILE)
            { o = 0;
              for (t = 0; t < NTHREADS; t++)
                { parmk[t].kidx = o;
                  for (j = 0; j < 256; j++)
                    o += Panels[t].khist[j];
                }
            }

          o = 0;
          for (j = 0; j < 256; j++)
            for (t = 0; t < NTHREADS; t++)
              { x = Panels[t].khist[j];
                Panels[t].khist[j] = o;
                o += x;
              }
          skmers = o;
        }

        if (VERBOSE)
          { fprintf(stderr,"\r  Processing part %d: Sorting weighted k-mers",p); 
            Wkmers[p] = skmers;
            Ukmers[p] = kmers;
            fflush(stderr);
          }

        if (DO_PROFILE)
          if (ODD_PASS)
            { i_sort = Malloc(skmers*(CMER_WORD+KMER_WORD),"Weighted k-mer & inverse list");
              k_sort = i_sort + skmers*CMER_WORD;
            }
          else
            { k_sort = Malloc(skmers*(KMER_WORD+CMER_WORD),"Weighted k-mer & inverse list");
              i_sort = k_sort + skmers*KMER_WORD;
            }
        else
          k_sort = Malloc(skmers*KMER_WORD,"Weighted k-mer list");

        for (t = 0; t < NTHREADS; t++)
          { int j;

            parmk[t].sort   = s_sort;
            parmk[t].parts  = Sparts;
            parmk[t].beg    = Panels[t].beg;
            parmk[t].end    = Panels[t].end;
            parmk[t].off    = Panels[t].off;
            for (j = 0; j < 256; j++)
              parmk[t].fours[j] = k_sort + Panels[t].khist[j]*KMER_WORD;
          }

#if defined(DEBUG_KLIST) || defined(DEBUG_CANONICAL)
        for (t = 0; t < NTHREADS; t++)
          kmer_list_thread(parmk+t);
#else
        for (t = 1; t < NTHREADS; t++)
          pthread_create(threads+t,NULL,kmer_list_thread,parmk+t);
	kmer_list_thread(parmk);
        for (t = 1; t < NTHREADS; t++)
          pthread_join(threads[t],NULL);
#endif

        //  Sort weighted k-mer list

        { uint8 *o, *x;
          int    j;

          o = k_sort;
          for (j = 0; j < 256; j++)
            { x = parmk[NTHREADS-1].fours[j];
              Kparts[j] = x-o;
              o = x;
            }
        }

        Weighted_Kmer_Sort(k_sort,skmers,KMER_WORD,KMER_BYTES,Kparts,NTHREADS,Panels);

        //  Accumulate accross threads the frequency histogram

        { int64 *ncnt;
          int    x;

          for (t = 0; t < NTHREADS; t++)
            { ncnt = Panels[t].count;
              for (x = 1; x < 0x8000; x++)
                counts[x] += ncnt[x];
            }
        }

        if (DO_TABLE > 0)
          {
            //  Threaded write of sorted kmer+count table
            //    1st time determine partition bytes values.  Therafter, spit on said

            if (p == 0)
              { for (t = 0; t < NTHREADS; t++)
                  { Table_Split[t] = Panels[t].beg;
                    parmt[t].off = Panels[t].off;
                  }
              }
            else
              { int64 off;
                int   beg;

                off = 0;
                beg = 0;
                for (t = 0; t < NTHREADS; t++)
                  { while (beg < Table_Split[t])
                      { off += Kparts[beg];
                        beg += 1;
                      }
                    parmt[t].off = off;
                  }
              }

            for (t = 0; t < NTHREADS; t++)
              { parmt[t].sort  = k_sort;
                parmt[t].parts = Kparts;
                parmt[t].beg   = Table_Split[t];
                if (t < NTHREADS-1)
                  parmt[t].end  = Table_Split[t+1];
                else
                  parmt[t].end = 256;
                sprintf(fname,"%s/%s.%d.L%d",SORT_PATH,dbrt,p,t);
                parmt[t].kfile = open(fname,O_WRONLY|O_CREAT|O_TRUNC,S_IRWXU|S_IRWXG|S_IRWXO);
              }

#ifdef DEBUG_TABOUT
            for (t = 0; t < NTHREADS; t++)
              table_write_thread(parmt+t);
#else
            for (t = 1; t < NTHREADS; t++)
              pthread_create(threads+t,NULL,table_write_thread,parmt+t);
	    table_write_thread(parmt);
            for (t = 1; t < NTHREADS; t++)
              pthread_join(threads[t],NULL);
#endif
          }

        if (! DO_PROFILE)
	  { free(k_sort);
            continue;
          }

        //  Fill in count/index list from sorted k-mer list, pre-sorted on
        //    LSD byte of index.

        { int64 o, x;
          int   j;

          o = 0;
          for (j = 0; j < 256; j++)
            for (t = 0; t < NTHREADS; t++)
              { x = Panels[t].khist[j];
                Panels[t].khist[j] = o;
                o += x;
              }
          if (o != skmers)
            { fprintf(stderr,"o != skmers (%lld vs %lld)\n",o,skmers);
              exit (1);
            }
        }

        if (VERBOSE)
          { fprintf(stderr,"\r  Processing part %d: Inverse profile sorting",p); 
            fflush(stderr);
          }

        for (t = 0; t < NTHREADS; t++)
          { int j;

            parmc[t].sort   = k_sort;
            parmc[t].parts  = Kparts;
            parmc[t].beg    = Panels[t].beg;
            parmc[t].end    = Panels[t].end;
            parmc[t].off    = Panels[t].off;
            for (j = 0; j < 256; j++)
              parmc[t].fours[j] = i_sort + Panels[t].khist[j]*CMER_WORD;
          }

#ifdef DEBUG_CLIST
        for (t = 0; t < NTHREADS; t++)
          cmer_list_thread(parmc+t);
#else
        for (t = 1; t < NTHREADS; t++)
          pthread_create(threads+t,NULL,cmer_list_thread,parmc+t);
        cmer_list_thread(parmc);
        for (t = 1; t < NTHREADS; t++)
          pthread_join(threads[t],NULL);
#endif

        //  LSD sort count/index list on index and then tidy up memory

        { int i, x;
          int bytes[KMAX_BYTES+1];

          x = 0;
          for (i = KMAX_BYTES; i >= 2; i--)
            bytes[x++] = i;
          bytes[x] = -1;

          i_sort = LSD_Sort(skmers,i_sort,k_sort,CMER_WORD,bytes);

          if (ODD_PASS)
            i_sort = Realloc(i_sort,skmers*CMER_WORD,"Pruning count list");
          else
            i_sort = Realloc(k_sort,skmers*CMER_WORD,"Pruning count list");
        }

        //  Use i_sort & k_sort again to build list of compressed profile fragments
        //    in place in i_sort, and build reference list

        p_sort = Malloc(nmers*PROF_BYTES*2,"Allocating profile link array");

        for (t = 0; t < NTHREADS; t++)
          { parmp[t].sort   = s_sort;
            parmp[t].parts  = Sparts;
            parmp[t].beg    = parmk[t].beg;
            parmp[t].end    = parmk[t].end;
            parmp[t].off    = parmk[t].off;
            parmp[t].prol   = i_sort;
            parmp[t].cnts   = parmk[t].kidx;
            parmp[t].fill   = p_sort + (parmk[t].off / SMER_WORD) * PROF_BYTES;
          }

#if defined(DEBUG_PLIST) || defined(SHOW_RUN)
        for (t = 0; t < NTHREADS; t++)
          profile_list_thread(parmp+t);
#else
        for (t = 1; t < NTHREADS; t++)
          pthread_create(threads+t,NULL,profile_list_thread,parmp+t);
        profile_list_thread(parmp);
        for (t = 1; t < NTHREADS; t++)
          pthread_join(threads[t],NULL);
#endif

        //  LSD sort profile links on super-mer idx

        { int   i, x;
          int   bytes[PROF_BYTES+1];

          x = 0;
          for (i = PROF_BYTES-1; i >= (int) sizeof(uint64); i--)
            bytes[x++] = i;
          bytes[x] = -1;

          a_sort = LSD_Sort(nmers,p_sort,p_sort+nmers*PROF_BYTES,PROF_BYTES,bytes);
        }

        //  Output profile fragments in order of a_sort links

        { int64 o;

          sprintf(fname,"%s/%s.%d.P",SORT_PATH,dbrt,p);
          o = 0;
          for (t = 0; t < NTHREADS; t++)
            { parmw[t].sort  = a_sort;
              parmw[t].beg   = o;
              o += parms[t].nmers;
              parmw[t].end   = o;
#ifdef DEBUG_PWRITE
              printf("Partition %2d: %10lld [%lld]\n",t,o,nmers);
#endif
              parmw[t].prol  = i_sort;
              parmw[t].root  = fname;
              parmw[t].wch   = t;
            }
        }

#ifdef DEBUG_PWRITE
        for (t = 0; t < NTHREADS; t++)
          profile_write_thread(parmw+t);
#else
        for (t = 1; t < NTHREADS; t++)
          pthread_create(threads+t,NULL,profile_write_thread,parmw+t);
        profile_write_thread(parmw);
        for (t = 1; t < NTHREADS; t++)
          pthread_join(threads[t],NULL);
#endif

	free(p_sort);
	free(i_sort);
      }

    free(s_sort);

    if (VERBOSE)
      { int64  wtot, utot;
        double psav;
        int    wwide, awide;

        wtot = utot = 0;
        psav = 0.;
        for (p = 0; p < NPARTS; p++)
          { wtot += Wkmers[p];
            if ((1.*Ukmers[p])/Wkmers[p] > psav)
              psav = (1.*Ukmers[p])/Wkmers[p];
            utot += Ukmers[p];
          }
        if ((1.*utot)/wtot > psav)
          psav = (1.*utot)/wtot;

        wwide = Number_Digits(wtot);
        awide = Number_Digits((int64) psav); 
        wwide += (wwide-1)/3;
        awide += 2;
        if (12 > wwide)
          wwide = 12;
        if (7 > awide)
          awide = 7;

        fprintf(stderr,"\r                                             \r"); 
        fprintf(stderr,"      Part:%*swgt'd k-mers%*ssavings\n",wwide-10,"",awide-5,"");
        fflush(stderr);
        for (p = 0; p < NPARTS; p++)
          { fprintf(stderr,"     %5d:  ",p);
            Print_Number(Wkmers[p],wwide,stderr);
            fprintf(stderr,"  %*.1f\n",awide,(1.*Ukmers[p])/Wkmers[p]);
          }
        fprintf(stderr,"       All:  ");
        Print_Number(wtot,wwide,stderr);
        fprintf(stderr,"  %*.1f\n",awide,(1.*utot)/wtot);
        fflush(stderr);
      }
  }


  //  Output histogram

  { int   i, f;
    int64 oob; 

    if (HIST_LOW > 0)
      { printf("\nHistogram of %d-mers:\n",KMER);
        oob = 0;
        for (i = 0x7fff; i > HIST_HGH; i--)
          oob += counts[i];
        if (i == 0x7fff)
          { if (counts[i] == 0)
              printf("    %5d: %12lld\n",i,counts[i]);
            else
              printf(" >= %5d: %12lld\n",i,counts[i]);
            i -= 1;
          }
        else if (oob > 0)
          printf(" >= %5d: %12lld\n",i+1,oob);
        for ( ; i >= HIST_LOW; i--)
          printf("    %5d: %12lld\n",i,counts[i]);
      }

    sprintf(fname,"%s/%s.K%d",dpwd,dbrt,KMER);
    f = open(fname,O_WRONLY|O_CREAT|O_TRUNC,S_IRWXU|S_IRWXG|S_IRWXO);
    write(f,&KMER,sizeof(int));
    write(f,counts,0x8000*sizeof(int64));
    close(f);
  }

  free(fname);
}