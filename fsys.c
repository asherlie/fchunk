#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>

#include "tc_stack.h"
#include "fname_hash.h"
#include "fsys.h"

struct fname* fn;

void fsys_init(struct fsys* fs){
      fs->n = 0;
      fs->cap = 10;
      fs->files = malloc(sizeof(struct finf)*fs->cap);
}

struct finf finf_build(time_t edit_t, ino_t file_no, char* fname){
      struct finf f;
      f.edit_t = edit_t;
      f.file_no = file_no;
      add_file_to_fhash(fn, file_no, strndup(fname, NAME_MAX));
      return f;
}

_Bool fsys_insert(struct fsys* fs, struct finf f){
      _Bool resize;
      if((resize = (fs->n == fs->cap))){
            fs->cap *= 2;
            struct finf* tmp_finf = malloc(sizeof(struct finf)*fs->cap);
            memcpy(tmp_finf, fs->files, sizeof(struct finf)*fs->n);
            free(fs->files);
            fs->files = tmp_finf;
      }
      fs->files[fs->n++] = f;
      return resize;
}

// if(!fs) a new fsys* is malloc'd
struct fsys* fsys_build(struct fsys* fs, char* fpath){
      if(!fpath)return NULL;
      _Bool m = 0;
      if(!fs){
            m = 1;
            fs = malloc(sizeof(struct fsys));
      }
      fsys_init(fs);
      struct stat attr;
      DIR* d = opendir(fpath);
      struct dirent* dir;
      if(d){
            while((dir = readdir(d))){
                  stat(dir->d_name, &attr);
                  fsys_insert(fs, finf_build(attr.st_mtime, dir->d_fileno, dir->d_name));
            }
            closedir(d);
      }
      else{
            if(m)free(fs);
            return NULL;
      }
      return fs;
}

struct fsys_cmp_in* fci_init(struct fsys_cmp_in* fci){
      if(!fci)fci = malloc(sizeof(struct fsys_cmp_in));
      fci->n = 0;
      fci->bux = 100;
      // TODO: free this
      fci->bucket_ind = malloc((fci->bux+1)*sizeof(int));
      // TODO: is this UB? man memset says `c` should be a byte
      memset(fci->bucket_ind, -1, sizeof(int)*(fci->bux+1));
      fci->cmp_entries = calloc(sizeof(struct fsys_cmp_entry), fci->bux);
      for(int i = 0; i < fci->bux; ++i){
            fci->cmp_entries[i].next = fci->cmp_entries[i].first =
            fci->cmp_entries[i].last = NULL;
      }

      return fci;
}

void fce_add_inf(struct fsys_cmp_in* fci, ino_t key, time_t edit_t, int age){
      int i = key%fci->bux;
      struct fsys_cmp_entry* fce = NULL;
      // if first entry in bucket
      if(!fci->cmp_entries[i].first && !fci->cmp_entries[i].last && !fci->cmp_entries[i].next){
            // TODO: fci->n is 2n
            fci->bucket_ind[fci->n] = i;
            fci->cmp_entries[i].first = malloc(sizeof(struct fsys_cmp_entry));
            fci->cmp_entries[i].last = fci->cmp_entries[i].first;
            fce = fci->cmp_entries[i].first;
      }
      else{
            for(fce = fci->cmp_entries[i].first; fce; fce = fce->next){
                  if(fce->key == key)break;
            }
            // if file not found
            if(!fce){
                  if(!fci->cmp_entries[i].first){
                        fci->cmp_entries[i].first = malloc(sizeof(struct fsys_cmp_entry));
                        fci->cmp_entries[i].last = fci->cmp_entries[i].first;
                        fce = fci->cmp_entries[i].first;
                  }
                  else{
                        fci->cmp_entries[i].last->next = malloc(sizeof(struct fsys_cmp_entry));
                        fci->cmp_entries[i].last = fci->cmp_entries[i].last->next;
                        fce = fci->cmp_entries[i].last;
                  }
            }
      }
      
      fce->next = NULL;
      fce->key = key;
      fce->edit_t[age] = edit_t;
      if(age == NEW)fce->old = 1;
      else fce->new = 1;
      fce->alt = (fce->old ^ fce->new) || (fce->old && fce->new && fce->edit_t[0] != fce->edit_t[1]);
      if(fce->alt){
            if(fce->old && fce->new)fce->alt_type = ALT_CHA;
            else{
                  if(fce->old && !fce->new)fce->alt_type = ALT_DEL;
                  else fce->alt_type = ALT_CRE;
            }

      }
      ++fci->n;

      return;
}

// returns a malloc'd struct fsys_cmp_in* comparing fs_new and fs_old
struct fsys_cmp_in* build_fci(struct fsys* fs_new, struct fsys* fs_old){
      struct fsys_cmp_in* ret = malloc(sizeof(struct fsys_cmp_in));
      fci_init(ret);
      // old entries must be added first
      for(int i = 0; i < fs_old->n; ++i)
            fce_add_inf(ret, fs_old->files[i].file_no, fs_old->files[i].edit_t, OLD);
      for(int i = 0; i < fs_new->n; ++i)
            fce_add_inf(ret, fs_new->files[i].file_no, fs_new->files[i].edit_t, NEW);
      return ret;
}

void fsys_cmp_free(struct fsys_cmp_in* fci){
      for(int i = 0; fci->bucket_ind[i] != -1; ++i){
            struct fsys_cmp_entry* prev = NULL;
            for(struct fsys_cmp_entry* fce = fci->cmp_entries[i].first; fce; fce = fce->next){
                  if(prev)free(prev);
                  prev = fce;
            }
            free(prev);
      }
      free(fci->cmp_entries);
      // TODO: why does this next line seg fault?
      // free(fci->bucket_ind);
}

// returns a malloc'd struct fsys_cmp_in*
// returns NULL if no change detected
// TODO: fsys_cmp should possibly take in tc_stack*
// struct fsys_cmp_in* fsys_cmp(struct fsys* fs_new, struct fsys* fs_old, int* n_alt){
// tc_stack should be initialized
struct fsys_cmp_in* fsys_cmp(struct fsys* fs_new, struct fsys* fs_old, struct tc_stack* tcs){
      struct fsys_cmp_in* fci = build_fci(fs_new, fs_old);

      for(int i = 0; fci->bucket_ind[i] != -1; ++i){
            for(struct fsys_cmp_entry* fce = fci->cmp_entries[fci->bucket_ind[i]].first;
                fce; fce = fce->next){
                  if(fce->alt)tc_stack_push(tcs, fce->key, fce->alt_type);
            }
      }

      // TODO: fci->n is twice what it should be
      // if(!fci->n || !*n_alt){
      if(!fci->n || !tcs->n){
            fsys_cmp_free(fci);
            free(fci);
            fci = NULL;
      }
      return fci;
}

void fsys_free(struct fsys* fs){
      free(fs->files);
}

// resolution is time to sleep between checks in usecs
// this is meant to be called from pthread_create()
void* track_changes_pth(void* tca_v){
      struct tc_arg* tca = (struct tc_arg*)tca_v;
      struct fsys* fs_o = malloc(sizeof(struct fsys));
      struct fsys* tmp_fs = malloc(sizeof(struct fsys));

      struct fsys_cmp_in* cmp;

      while(*tca->run){
            fsys_build(fs_o, tca->fpath);
            usleep(tca->res);
            fsys_build(tmp_fs, tca->fpath);

            // should fsys_cmp take an int* for alt_type
            // if((cmp = fsys_cmp(fs_o, tmp_fs, &diff))){
            if((cmp = fsys_cmp(fs_o, tmp_fs, tca->tc_stack))){
                  // free cmp instantly - waste of mallocs...
                  fsys_cmp_free(cmp);
                  free(cmp);
            }
            fsys_free(fs_o);
            fsys_free(tmp_fs);
      }
      free(fs_o);
      free(tmp_fs);
      return NULL;
}


struct track_chng track_changes(char* fpath, int res){
      fn = fname_init(fn, 100);
      struct tc_arg* tca = malloc(sizeof(struct tc_arg));
      tca->run = malloc(sizeof(_Bool));
      *tca->run = 1;
      tca->res = res;
      tca->fpath = fpath;
      pthread_t pt; (void)pt;

      tca->tc_stack = tc_stack_init(NULL);
      pthread_mutex_init(&tca->tc_stack->tc_stack_mut, NULL);

      struct track_chng tc;
      tc.run = tca->run;
      tc.tca = tca;
      tc.fname_hash = fn;

      tc.tc_stack = tc_stack_init(NULL);
      tc.tc_stack = tca->tc_stack;

      pthread_create(&pt, NULL, &track_changes_pth, tca);
      tc.pth = pt;
      return tc;
}

void untrack_changes(struct track_chng tc){
      pthread_mutex_destroy(&tc.tc_stack->tc_stack_mut);
      tc_stack_free(tc.tc_stack);
      free(tc.tc_stack);
      free(tc.tca);
      *tc.run = 0;
      pthread_join(tc.pth, NULL);
      free(tc.run);
      fhash_free(fn);
}
