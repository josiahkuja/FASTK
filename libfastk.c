/*******************************************************************************************
 *
 *  C library routines to access and operate upon FastK histogram, k-mer tables, and profiles
 *
 *  Author:  Gene Myers
 *  Date  :  November 2020
 *
 *******************************************************************************************/

#include "libfastk.h"

#include "gene_core.c"

/*********************************************************************************************\
 *
 *  HISTOGRAM CODE
 *
 *********************************************************************************************/

Histogram *Load_Histogram(char *name, int low, int high)
{ Histogram *H;
  int        kmer, flow, fhigh;
  int64     *hist, under, over, count;
  char      *dir, *root;
  FILE      *f;
  int        i;

  dir  = PathTo(name);
  root = Root(name,".hist");
  f = fopen(Catenate(dir,"/",root,".hist"),"r");
  if (f == NULL)
    { fprintf(stderr,"Cannot open %s for reading\n",Catenate(dir,"/",root,".hist"));
      exit (1);
    }
  free(root);
  free(dir);

  fread(&kmer,sizeof(int),1,f);
  fread(&flow,sizeof(int),1,f);
  fread(&fhigh,sizeof(int),1,f);

  if (flow > low || fhigh < high)
    { fprintf(stderr,"Histogram range [%d,%d] is not a superset of requested range [%d,%d]\n",
                     flow,fhigh,low,high);
      fclose(f);
      exit (1);
    }

  hist = Malloc(sizeof(int64)*((high-low)+1),"Allocating histogram");
  
  under = 0;
  for (i = flow; i < low; i++)
    { fread(&count,sizeof(int64),1,f);
      under += count;
    }
  fread(hist,sizeof(int64),(high-low)+1,f);
  over = 0;
  for (i = high+1; i < fhigh; i++)
    { fread(&count,sizeof(int64),1,f);
      over += count;
    }
    
  fclose(f);

  H = Malloc(sizeof(Histogram),"Allocating histogram");
  H->kmer = kmer;
  H->low  = low;
  H->high = high;
  H->hist = hist-low;

  hist[0]  += under;
  hist[high-low] += over;

  return (H);
}

void Free_Histogram(Histogram *H)
{ free(H->hist+H->low);
  free(H);
}

/****************************************************************************************
 *
 *  K-MER TABLE CODE
 *
 *****************************************************************************************/

/****************************************************************************************
 *
 *  Print & compare utilities
 *
 *****************************************************************************************/

#define  KMER(i)  (table+(i)*tbyte)
#define  COUNT(i) (*((uint16 *) (table+(i)*tbyte+kbyte)))

static char dna[4] = { 'a', 'c', 'g', 't' };

static char *fmer[256], _fmer[1280];

static void setup_fmer_table()
{ char *t;
  int   i, l3, l2, l1, l0;

  i = 0;
  t = _fmer;
  for (l3 = 0; l3 < 4; l3++)
   for (l2 = 0; l2 < 4; l2++)
    for (l1 = 0; l1 < 4; l1++)
     for (l0 = 0; l0 < 4; l0++)
       { fmer[i] = t;
         *t++ = dna[l3];
         *t++ = dna[l2];
         *t++ = dna[l1];
         *t++ = dna[l0];
         *t++ = 0;
         i += 1;
       }
}

static void print_seq(uint8 *seq, int len)
{ int i, b, k;

  b = len >> 2;
  for (i = 0; i < b; i++)
    printf("%s",fmer[seq[i]]);
  k = 6;
  for (i = b << 2; i < len; i++)
    { printf("%c",dna[seq[b] >> k]);
      k -= 2;
    }
}

static void print_pack(uint8 *seq, int len)
{ int i;

  for (i = 0; i < (len+3)/4; i++)
    printf(" %02x",seq[i]);
}
  
static inline int mycmp(uint8 *a, uint8 *b, int n)
{ while (n--)
    { if (*a++ != *b++)
        return (a[-1] < b[-1] ? -1 : 1);
    }
  return (0);
}

static inline void mycpy(uint8 *a, uint8 *b, int n)
{ while (n--)
    *a++ = *b++;
}


/****************************************************************************************
 *
 *  Read in a table and return as Kmer_Table object
 *
 *****************************************************************************************/

Kmer_Table *Load_Kmer_Table(char *name, int cut_freq, int smer, int nthreads)
{ Kmer_Table *T;
  int         kmer, tbyte, kbyte;
  int64       nels;
  uint8      *table;

  FILE  *f;
  int    p;
  int64  n;

  setup_fmer_table();

  //  Find all parts and accumulate total size

  nels = 0;
  for (p = 1; p <= nthreads; p++)
    { f = fopen(Catenate(name,Numbered_Suffix(".ktab.",p,""),"",""),"r");
      if (f == NULL)
        { fprintf(stderr,"Table part %s.ktab.%d is misssing ?\n",name,p);
          exit (1);
        }
      fread(&kmer,sizeof(int),1,f);
      fread(&n,sizeof(int64),1,f);
      nels += n;
      if (kmer != smer)
        { fprintf(stderr,"Table part %s.ktab.%d does not have k-mer length matching stub ?\n",
                         name,p);
          exit (1);
        }
      fclose(f);
    }

  //  Allocate in-memory table

  kbyte = (kmer+3)>>2;
  tbyte = kbyte+2;
  table = Malloc(nels*tbyte,"Allocating k-mer table\n");
  if (table == NULL)
    exit (1);

  //  Load the parts into memory

  fprintf(stderr,"Loading %d-mer table with ",kmer);
  Print_Number(nels,0,stderr);
  fprintf(stderr," entries in %d parts\n",p-1);
  fflush(stderr);

  nels = 0;
  for (p = 1; p <= nthreads; p++)
    { f = fopen(Catenate(name,Numbered_Suffix(".ktab.",p,""),"",""),"r");
      fread(&kmer,sizeof(int),1,f);
      fread(&n,sizeof(int64),1,f);
      fread(KMER(nels),n*tbyte,1,f);
      nels += n;
      fclose(f);
    }

  if (cut_freq > 1)
    { int64 i, j;

      j = 0;
      for (i = 0; i < nels; i++)
        if (COUNT(i) >= cut_freq)
          mycpy(KMER(j++),KMER(i),tbyte);
      if (j < nels)
        { nels = j;
          table = Realloc(table,nels*tbyte,"Reallocating table");
        }
    }

  T = Malloc(sizeof(Kmer_Table),"Allocating table record");
  if (T == NULL)
    exit (1);

  T->kmer  = kmer;
  T->tbyte = tbyte;
  T->kbyte = kbyte;
  T->nels  = nels;
  T->table = table;
  T->index = NULL;

  return (T);
}


/****************************************************************************************
 *
 *  Free, Fetch, Check, and List a Table
 *
 *****************************************************************************************/

char *Fetch_Kmer(Kmer_Table *T, int i)
{ static char *seq = NULL;
  static int   max = 0;

  int    kmer  = T->kmer;
  int    tbyte = T->tbyte;
  uint8 *table = T->table;

  if (i < 0)
    { free(seq);
      max = 0;
      return (NULL);
    }

  if (kmer > max)
    { max = kmer;
      seq = Realloc(seq,max+1,"Reallocating k-mer buffer");
      if (seq == NULL)
        exit (1);
    }

  { int    j, k;
    uint8 *a;

    a = KMER(i);
    for (j = 0; j < kmer; j += 4)
      sprintf(seq+j,"%s",fmer[*a++]);
    k = 6;
    for (j -= 4; j < kmer; j++)
      { seq[j] = dna[*a >> k];
        k -= 2;
      }
  }

  return (seq);
}

int Fetch_Count(Kmer_Table *T, int i)
{ int    kbyte = T->kbyte;
  int    tbyte = T->tbyte;
  uint8 *table = T->table;

  return (COUNT(i));
}

void Free_Kmer_Table(Kmer_Table *T)
{ free(T->table);
  free(T);
}

void Check_Kmer_Table(Kmer_Table *T)
{ int    kmer  = T->kmer;
  int    tbyte = T->tbyte;
  int    kbyte = T->kbyte;
  int64  nels  = T->nels;
  uint8 *table = T->table;
  
  int i;

  setup_fmer_table();

  printf("\n");
  for (i = 1; i < nels; i++)
    { if (mycmp(KMER(i-1),KMER(i),kbyte) >= 0)
        { printf("Out of Order\n");
          printf(" %9d:",i-1);
          print_pack(KMER(i-1),kmer);
          printf("  ");
          print_seq(KMER(i-1),kmer);
          printf(" = %4d\n",COUNT(i-1));
          printf(" %9d:",i);
          print_pack(KMER(i),kmer);
          printf("  ");
          print_seq(KMER(i),kmer);
          printf(" = %4d\n",COUNT(i));
          break;
        }
    }
  if (i >= nels)
    printf("Table is OK\n");
}

void List_Kmer_Table(Kmer_Table *T)
{ int    kmer  = T->kmer;
  int    tbyte = T->tbyte;
  int    kbyte = T->kbyte;
  int64  nels  = T->nels;
  uint8 *table = T->table;

  int i;

  setup_fmer_table();

  printf("\nElement Bytes = %d  Kmer Bytes = %d\n",tbyte,kbyte);

  printf(" %9d: ",0);
  print_seq(KMER(0),kmer);
  printf(" = %4d\n",COUNT(0));
  for (i = 1; i < nels; i++)
    { if (mycmp(KMER(i-1),KMER(i),kbyte) >= 0)
        printf("Out of Order\n");
      printf(" %9d: ",i);
      print_seq(KMER(i),kmer);
      printf(" = %4d\n",COUNT(i));
    }
}


/****************************************************************************************
 *
 *  Find k-mer in table
 *
 *****************************************************************************************/

static void set_up_accelerator(Kmer_Table *T)
{ int     tbyte = T->tbyte;
  int64   nels  = T->nels;
  uint8  *table = T->table;
  int64  *index;

  uint8 *iptr, *nptr;
  int64  i;
  int    idx, val;

  index = Malloc(sizeof(uint8 *)*0x1000001,"Allocating acceleraator");
  if (index == NULL)
    exit (1);

  idx  = 1;
  iptr = table;
  nptr = KMER(nels);
  index[0] = 0;
  for (i = 1, iptr += tbyte; iptr < nptr; i++, iptr += tbyte)
    { if (mycmp(iptr,iptr-tbyte,3) == 0)
        continue;
      val = (iptr[0] << 16) | (iptr[1] << 8) | iptr[2];
      while (idx <= val)
        index[idx++] = i;
    }
  while (idx <= 0x1000000)
    index[idx++] = nels;

  T->index = index;
}

static uint8 code[128] =
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static uint8 comp[128] =
  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 3, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 3, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static int is_minimal(char *seq, int len)
{ int j, k;
  int x, y;
  
  for (k = 0, j = len-1; k < j; k++, j--)
    { x = code[(int) seq[k]];
      y = comp[(int) seq[j]];
      if (x < y)
        return (1);
      if (x > y)
        return (0);
    }
  if (k <= j)
    { x = code[(int) seq[k]];
      if (x < 2)
        return (1);
      else
        return (0);
    }
  else
    return (1);
}

static void compress_norm(char *s, int len, uint8 *t)
{ int    i;
  char   c, d, e;
  char  *s0, *s1, *s2, *s3;

  s0 = s;
  s1 = s0+1;
  s2 = s1+1;
  s3 = s2+1;

  c = s0[len];
  d = s1[len];
  e = s2[len];
  s0[len] = s1[len] = s2[len] = 0;


  for (i = 0; i < len; i += 4)
    *t++ = ((code[(int) s0[i]] << 6) | (code[(int) s1[i]] << 4)
         |  (code[(int) s2[i]] << 2) | code[(int) s3[i]] );

  s0[len] = c;
  s1[len] = d;
  s2[len] = e;
}

static void compress_comp(char *s, int len, uint8 *t)
{ int    i;
  char   c, d, e;
  char  *s0, *s1, *s2, *s3;

  s0 = s;
  s1 = s0-1;
  s2 = s1-1;
  s3 = s2-1;

  c = s0[0];
  d = s1[0];
  e = s2[0];
  s0[0] = s1[0] = s2[0] = 3;

  for (i = len-1; i >= 0; i -= 4)
    *t++ = ((comp[(int) s0[i]] << 6) | (comp[(int) s1[i]] << 4)
         |  (comp[(int) s2[i]] << 2) | comp[(int) s3[i]] );

  s0[0] = c;
  s1[0] = d;
  s2[0] = e;
}

int Find_Kmer(Kmer_Table *T, char *kseq)
{ int    kmer  = T->kmer;
  int    tbyte = T->tbyte;
  int    kbyte = T->kbyte;
  int64  nels  = T->nels;
  uint8 *table = T->table;

  uint8  cmp[kbyte];
  int64  l, r, m;

  //  kseq must be at least kmer bp long

  if (is_minimal(kseq,kmer))
    compress_norm(kseq,kmer,cmp);
  else
    compress_comp(kseq,kmer,cmp);

  if (kbyte >= 3)
    { int64 *index = T->index;
      if (index == NULL)
        { set_up_accelerator(T);
          index = T->index;
        }
      m = (cmp[0] << 16) | (cmp[1] << 8) | cmp[2];
      l = index[m];
      r = index[m+1];
    }
  else
    { l = 0;
      r = nels;
    }

  // smallest l s.t. KMER(l) >= (kmer) cmp  (or nels if does not exist)

  while (l < r)
    { m = ((l+r) >> 1);
      if (mycmp(KMER(m),cmp,kbyte) < 0)
        l = m+1;
      else
        r = m;
    }

  if (l >= nels || mycmp(KMER(l),cmp,kbyte) != 0)
    return (0);

  return (COUNT(l));
}


/*********************************************************************************************\
 *
 *  PROFILE CODE
 *
 *****************************************************************************************/

/****************************************************************************************
 *
 *  Open a profile as a Profile_Index.  Index to compressed profiles is in memory,
 *    but compressed profiles are left on disk and reaad only when requested.
 *
 *****************************************************************************************/

Profile_Index *Open_Profiles(char *name, int smer, int nthreads)
{ Profile_Index *P;
  int            kmer, nparts;
  int64          nreads, *nbase, *index;
  FILE         **nfile;

  FILE  *f;
  int64  n;

  //  Find all parts and accumulate total size

  nreads = 0;
  for (nparts = 0; nparts < nthreads; nparts++)
    { f = fopen(Catenate(name,Numbered_Suffix(".pidx.",nparts+1,""),"",""),"r");
      if (f == NULL)
        { fprintf(stderr,"Profile part %s.pidx.%d is misssing ?\n",name,nparts+1);
          exit (1);
        }
      fread(&kmer,sizeof(int),1,f);
      fread(&n,sizeof(int64),1,f);
      fread(&n,sizeof(int64),1,f);
      nreads += n;
      if (kmer != smer)
        { fprintf(stderr,"Profile part %s.pidx.%d does not have k-mer length matching stub ?\n",
                         name,nparts+1);
          exit (1);
        }
      fclose(f);
    }

  //  Allocate in-memory table

  index = Malloc((nreads+1)*sizeof(int64),"Allocating profile index");
  nbase = Malloc(nparts*sizeof(int64),"Allocating profile index");
  nfile = Malloc(nparts*sizeof(FILE *),"Allocating profile index");
  if (index == NULL || nbase == NULL || nfile == NULL)
    exit (1);

  nreads = 0;
  index[0] = 0;
  for (nparts = 0; nparts < nthreads; nparts++)
    { f = fopen(Catenate(name,Numbered_Suffix(".pidx.",nparts+1,""),"",""),"r");
      fread(&kmer,sizeof(int),1,f);
      fread(&n,sizeof(int64),1,f);
      fread(&n,sizeof(int64),1,f);
      fread(index+(nreads+1),sizeof(int64),n,f);
      nreads += n;
      nbase[nparts] = nreads;
      fclose(f);

      f = fopen(Catenate(name,Numbered_Suffix(".prof.",nparts+1,""),"",""),"r");
      if (f == NULL)
        { fprintf(stderr,"Profile part %s.prof.%d is misssing ?\n",name,nparts+1);
          exit (1);
        }
      nfile[nparts] = f;
    }

  P = Malloc(sizeof(Profile_Index),"Allocating profile record");
  if (P == NULL)
    exit (1);

  P->kmer   = kmer;
  P->nparts = nparts;
  P->nreads = nreads;
  P->index  = index;
  P->nbase  = nbase;
  P->nfile  = nfile;

  return (P);
}

/****************************************************************************************
 *
 *  Free a Profile_Index and fetch a profile
 *
 *****************************************************************************************/

#undef SHOW_RUN

void Free_Profiles(Profile_Index *P)
{ int i;

  free(P->index);
  free(P->nbase);
  for (i = 0; i < P->nparts; i++)
    fclose(P->nfile[i]);
  free(P->nfile);
  free(P);
}

  //  Places uncompressed profile for read id (0-based) in profile of length plen.
  //    Returns the length of the uncompressed profile.  If the plen is less than
  //    this then only the first plen counts are uncompressed into profile

int Fetch_Profile(Profile_Index *P, int64 id, int plen, uint16 *profile)
{ uint8 count[1000], *cend = count+999;
  FILE  *f;
  int    w, len;
  uint8 *p, *q;
  uint16 x, d, i;
  int    n;

  for (w = 0; w < P->nparts; w++)
    if (id < P->nbase[w])
      break;
  if (w >= P->nparts)
    { fprintf(stderr,"Id %lld is out of range [1,%lld]\n",id,P->nbase[P->nparts-1]);
      exit (1);
    }
  f = P->nfile[w];

  if (id == 0 || (w > 0 && id == P->nbase[w-1]))
    { fseek(f,0,SEEK_SET);
      len = P->index[id+1];
    }
  else
    { int64 off = P->index[id];
      fseek(f,off,SEEK_SET);
      len = P->index[id+1] - off;
    }

  fread(count,1000,1,f);

  p = count;
  q = count + len;

  x = *p++;
  if ((x & 0x80) != 0)
    d = ((x & 0x7f) << 8) | *p++;
  else
    d = x;
  n = 1;

  if (plen > 0)
    { profile[0] = d;
#ifdef SHOW_RUN
  printf(" %d\n",d);
#endif

      while (p < q)
        { if (p >= cend)
            { if (p == cend)
                { *count = *p; 
                  fread(count+1,999,1,f);
                  p  = count;
                  q -= 999;
                }
              else
                { fread(count,1000,1,f);
                  p = count;
                  q -= 1000;
                }
            }
          x = *p++;
          if ((x & 0xc0) == 0)
            { if (n+x > plen)
                { n += x;
                  break;
                }
              for (i = 0; i < x; i++)
                profile[n++] = d;
#ifdef SHOW_RUN
              printf(" [%hu]\n",x);
#endif
            }
          else
            { if ((x & 0x80) != 0)
                { if ((x & 0x40) != 0)
                    x <<= 8;
                  else
                    x = (x << 8) & 0x7fff;
                  x |= *p++;
                  d += x;
#ifdef SHOW_RUN
                  printf(" %hd+(%d)\n",x,d);
#endif
                }
              else
                { if ((x & 0x20) != 0)
                    d += (x & 0x1fu) | 0xffe0u;
                  else
                    d += (x & 0x1fu);
#ifdef SHOW_RUN
                  if ((x & 0x20) != 0)
                    printf(" -%d(%d)\n",32-(x&0x1fu),d);
                  else
                    printf(" +%d(%d)\n",x&0x1fu,d);
#endif
                }
              if (n >= plen)
                { n += 1;
                  break;
                }
              profile[n++] = d;
            }
        }
    }

  while (p < q)
    { if (p >= cend)
        { if (p == cend)
            { *count = *p; 
              fread(count+1,999,1,f);
              p  = count;
              q -= 999;
            }
          else
            { fread(count,1000,1,f);
              p = count;
              q -= 1000;
            }
        } 
      x = *p++;
      if ((x & 0xc0) == 0)
        n += x;
      else
        { if ((x & 0x80) != 0)
            p += 1;
          n += 1;
        }
    }

  return (n);
}