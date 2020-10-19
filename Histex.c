/*******************************************************************************************
 *
 *  Example code for reading and displatying a kmer histogram produced by FastK.
 *
 *  Author:  Gene Myers
 *  Date  :  October 2020
 *
 *******************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "gene_core.h"

#define HSIZE  0x8000

static char *Usage = " [-h[<int(1)>:]<int(100)>] <source_root>.K<k>";

int main(int argc, char *argv[])
{ int64 *cgram;
  int    kmer;

  int    HIST_LOW;
  int    HIST_HGH;

  //  Process arguments

  { int    i, j, k;
    int    flags[128];
    char  *eptr, *fptr;

    ARG_INIT("Histex")

    HIST_LOW    = 1;
    HIST_HGH    = 100;

    j = 1;
    for (i = 1; i < argc; i++)
      if (argv[i][0] == '-')
        switch (argv[i][1])
        { default:
            ARG_FLAGS("")
            break;
          case 'h':
            HIST_LOW = strtol(argv[i]+2,&eptr,10);
            if (eptr > argv[i]+2)
              { if (HIST_LOW < 1 || HIST_LOW > 0x7fff)
                  { fprintf(stderr,"%s: Histogram count %d is out of range\n",
                                   Prog_Name,HIST_LOW);
                    exit (1);
                  }
                if (*eptr == ':')
                  { HIST_HGH = strtol(eptr+1,&fptr,10);
                    if (fptr > eptr+1 && *fptr == '\0')
                      { if (HIST_LOW > HIST_HGH)
                          { fprintf(stderr,"%s: Histogram range is invalid\n",Prog_Name);
                            exit (1);
                          }
                        break;
                      }
                  }
                else if (*eptr == '\0')
                  { HIST_HGH = HIST_LOW;
                    HIST_LOW = 1;
                    break;
                  }
              }
            fprintf(stderr,"%s: Syntax of -h option invalid -h[<int(1)>:]<int>\n",Prog_Name);
            exit (1);
        }
      else
        argv[j++] = argv[i];
    argc = j;

    if (HIST_HGH > 0x7fff)
      HIST_HGH = 0x7fff;

    if (argc != 2)
      { fprintf(stderr,"Usage: %s %s\n",Prog_Name,Usage);
        fprintf(stderr,"\n");
        fprintf(stderr,"      -h: Output histogram of counts in range given\n");
        exit (1);
      }
  }

  //  Load histogram into "cgram"

  { FILE *f;

    f = fopen(argv[1],"r");
    if (f == NULL)
      { fprintf(stderr,"%s: Cannot open %s\n",Prog_Name,argv[1]);
        exit (1);
      }

    cgram = Malloc(sizeof(int64)*HSIZE,"Allocating histogram");

    fread(&kmer,sizeof(int),1,f);
    fread(cgram,sizeof(int64),HSIZE,f);
    
    fclose(f);
  }

  //  Generate display

  { char       *root;
    int         i;
    int64       ssum, stotal;

    root = Root(argv[1],NULL);
    printf("\nHistogram of %d-mers of %s\n",kmer,root);
    free(root);

    stotal = 0;
    for (i = 0; i < HSIZE; i++)
      stotal += cgram[i];

    printf("\n  Input: ");
    Print_Number(stotal,0,stdout);
    printf(" %d-mers\n",kmer);

    printf("\n     Freq:        Count   Cum. %%\n");
    ssum = 0;
    for (i = HSIZE-1; i >= HIST_LOW; i--)
      if (cgram[i] > 0)
        { ssum += cgram[i];
          if (i == HIST_HGH)
            { printf(" >= %5d: %12lld",i,ssum);
              printf("   %5.1f%%\n",(100.*ssum)/stotal);
            }
          else if (i < HIST_HGH)
            { printf("    %5d: %12lld",i,cgram[i]);
              printf("   %5.1f%%\n",(100.*ssum)/stotal);
            }
        }
    if (HIST_LOW > 1)
      printf("  < %5d: %12lld   100.0%%\n",i,stotal-ssum);
  }

  free(Prog_Name);
  exit (0);
}