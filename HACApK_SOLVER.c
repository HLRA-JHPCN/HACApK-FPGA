
#if defined(HAVE_MAGMA) | defined(HAVE_MAGMA_BATCH)

#include "HACApK_MAGMA.h"

// !! BiCG in C !!
double c_hacapk_dotp_d(int nd, double *b, double *a) {
    double norm = 0.0;
    int ii;
    for (ii=0; ii<nd; ii++) {
        norm += b[ii]*a[ii];
    }
    return norm;
}

#define PINNED_BUFFER
//#define PORTABLE_BUFFER
#if defined(PORTABLE_BUFFER)
magma_int_t
magma_dmalloc_pinned_portable( double** ptrPtr, size_t size )
{
    if (size == 0) size = 1;
    unsigned int flags = cudaHostAllocWriteCombined | cudaHostAllocPortable;
    if ( cudaSuccess != cudaHostAlloc( (void**)ptrPtr, size * sizeof(double), flags )) {
        return MAGMA_ERR_HOST_ALLOC;
    }
    return MAGMA_SUCCESS;
}
#endif

//#define WARMUP_MPI
void c_hacapk_adot_cax_lfmtx_warmup(stc_HACApK_lcontrol *st_ctl,
                                    double*zau, double *wws, double *wwr, int nd) {
    double tic;
    int *lpmd = (int*)((void*)st_ctl->param + st_ctl->lpmd_offset); 
    int mpinr = lpmd[2]; 
    int nrank = lpmd[1]; 
   
    if (nrank > 1) {
        int *lnp = (int*)((void*)st_ctl->param + st_ctl->lnp_offset);
        MPI_Comm icomm = MPI_COMM_WORLD;

        int ncdp = (mpinr+1)%nrank;       // my destination neighbor
        int ncsp = (mpinr+nrank-1)%nrank; // my source neighbor

        int ione = 1;
        double one = 1.0;
        lapackf77_dlaset( "F", &nd, &ione, &one, &one, zau, &nd );
        lapackf77_dlaset( "F", &nd, &ione, &one, &one, wws, &nd );
        lapackf77_dlaset( "F", &nd, &ione, &one, &one, wwr, &nd );
        {
           int nctp = (ncsp+nrank)%nrank; // where it came from

           double zero = 0.0;
           int send_count = nd; //lnp[nctp];
           int recv_count = nd; //lnp[mpinr];
           MPI_Status stats[2];
           MPI_Request reqs[2];
           //printf( " %d: send(%d/%d to %d), recv(%d/%d from %d)\n",mpinr,send_count,nd,ncdp,recv_count,nd,ncsp );
           MPI_Isend(wws, send_count, MPI_DOUBLE, ncdp, nrank+1, MPI_COMM_WORLD, &reqs[0]);
           MPI_Irecv(wwr, recv_count, MPI_DOUBLE, ncsp, nrank+1, MPI_COMM_WORLD, &reqs[1]);
           MPI_Waitall(2, reqs, stats);
           /*if (ncdp == 0) {
               printf( " TEST: wws(%d):",mpinr );
               magma_dprint(10,1,wws,send_count);
           }
           if (mpinr == 0) {
               printf( " TEST: wwr(%d):",mpinr );
               magma_dprint(10,1,wwr,recv_count);
           }*/
        }
    }
}

void c_hacapk_adot_cax_lfmtx_comm_setup(stc_HACApK_lcontrol *st_ctl,
                                        double **buffer, int **p_disps, magma_queue_t queue) {
    int *lpmd = (int*)((void*)st_ctl->param + st_ctl->lpmd_offset);
    int mpinr = lpmd[2];
    int nrank = lpmd[1];

    int buffer_size = 0;
    int *disps  = (int*)malloc((1+nrank) * sizeof(int));
    disps[0] = 0;
    if (nrank > 1) {
        int *lnp = (int*)((void*)st_ctl->param + st_ctl->lnp_offset);
        int ic;

        for (ic=0; ic<nrank; ic++) {
           disps[ic+1] = disps[ic]+lnp[ic];
        }
        buffer_size = disps[nrank];
    }
    *p_disps = disps;
    if (buffer_size > 0) {
        #if defined(USE_GDR)
        if (queue != NULL) {
            magma_dmalloc(buffer, buffer_size);
        } else
        #endif
        *buffer = (double*)malloc(buffer_size * sizeof(double));
    } else {
        *buffer = NULL;
    }
}

void c_hacapk_adot_cax_lfmtx_comm(double *zau, stc_HACApK_lcontrol *st_ctl, double* buffer, int *disps,
                                  double *wws, double *wwr, int *isct, int *irct, int nd, magma_queue_t queue,
                                  double *time_mpi) {
    int ione = 1;
    double one = 1.0;

    double tic;
    int *lpmd = (int*)((void*)st_ctl->param + st_ctl->lpmd_offset); 
    int mpinr = lpmd[2]; 
    int nrank = lpmd[1]; 
   
    if (nrank > 1) {
        int *lsp = (int*)((void*)st_ctl->param + st_ctl->lsp_offset);
        int *lnp = (int*)((void*)st_ctl->param + st_ctl->lnp_offset);
        MPI_Comm icomm = MPI_COMM_WORLD;

        int ic;
        int ncdp = (mpinr+1)%nrank;       // my destination neighbor
        int ncsp = (mpinr+nrank-1)%nrank; // my source neighbor
        #define COMM_BY_ALLGATHERV
        #if defined(COMM_BY_ALLGATHERV)
        #if defined(USE_GDR)
        magma_queue_sync( queue );
        #endif
        tic = MPI_Wtime();
        MPI_Allgatherv(&zau[lsp[mpinr]-1], lnp[mpinr], MPI_DOUBLE, buffer, lnp, disps, MPI_DOUBLE, MPI_COMM_WORLD);
        *time_mpi += (MPI_Wtime()-tic);

        #if defined(REPRODUCIBLE_SUM) // !! make sure "reproduciblity" with some extra flops !!
        double zero = 0.0;
        lapackf77_dlaset( "F", &lnp[mpinr], &ione, &zero, &zero, &zau[lsp[mpinr]-1], &nd );
        for (ic=0; ic<nrank; ic++) {
           irct[0] = lnp[ic];
           irct[1] = lsp[ic];
           blasf77_daxpy( &irct[0], &one, &buffer[disps[ic]], &ione, &zau[irct[1]-1], &ione );
        }
        #else
        for (ic=1; ic<nrank; ic++) {
           int nctp = (ncsp-ic+nrank+1)%nrank; // where it came from
           irct[0] = lnp[nctp];
           irct[1] = lsp[nctp];
           #if defined(USE_GDR)
           if (queue != NULL) {
               magma_daxpy( irct[0], one, &buffer[disps[nctp]], ione, &zau[irct[1]-1], ione, queue );
           } else
           #endif
           blasf77_daxpy( &irct[0], &one, &buffer[disps[nctp]], &ione, &zau[irct[1]-1], &ione );
        }
        #endif

        #else
        isct[0] = lnp[mpinr];
        isct[1] = lsp[mpinr];

        // copy local vector to send buffer
        dlacpy_( "F", &lnp[mpinr], &ione, &zau[lsp[mpinr]-1], &lnp[mpinr], wws, &lnp[mpinr] );
        for (ic=1; ic<nrank; ic++) {
           MPI_Status stat;
           tic = MPI_Wtime();
           #if 0
           MPI_Sendrecv(isct, 2, MPI_INT, ncdp, 2*(ic-1),
                        irct, 2, MPI_INT, ncsp, 2*(ic-1), 
                        icomm, &stat);
           #else // read offset/size from structure
           int nctp = (ncsp-ic+nrank+1)%nrank; // where it came from
           irct[0] = lnp[nctp];
           irct[1] = lsp[nctp];
           #endif

           //#define COMM_BY_ISEND_IRECV
           #if defined(COMM_BY_ISEND_IRECV)
           MPI_Status stats[2];
           MPI_Request reqs[2];
           if (MPI_SUCCESS != MPI_Isend(wws, isct[0], MPI_DOUBLE, ncdp, nrank+ic, MPI_COMM_WORLD, &reqs[0])) 
               printf( "MPI_Isend failed\n" );
           if (MPI_SUCCESS != MPI_Irecv(wwr, irct[0], MPI_DOUBLE, ncsp, nrank+ic, MPI_COMM_WORLD, &reqs[1]))
               printf( "MPI_Irecv failed\n" );
           if (MPI_SUCCESS != MPI_Waitall(2, reqs, stats))
               printf( "MPI_Waitall failed\n" );
           #else
           int info = 
           MPI_Sendrecv(wws, isct[0], MPI_DOUBLE, ncdp, 2*(ic-1)+1,
                        wwr, irct[0], MPI_DOUBLE, ncsp, 2*(ic-1)+1,
                        icomm, &stat);
           if (info != MPI_SUCCESS) printf( " MPI_Sendrecv failed with info=%d\n",info );
           #endif
           *time_mpi += (MPI_Wtime()-tic);
           blasf77_daxpy( &irct[0], &one, wwr, &ione, &zau[irct[1]-1], &ione );

           dlacpy_( "F", &irct[0], &ione, wwr, &irct[0], wws, &irct[0] );
           isct[0] = irct[0];
           isct[1] = irct[1];
        }
        #endif
    }
}

void c_hacapk_adot_cax_lfmtx_comm_gpu(int flag, double *zau_gpu, double *zau,
                                      stc_HACApK_lcontrol *st_ctl, double *buffer, int *disps,
                                      double *wws, double *wwr, int *isct, int *irct, int nd, 
                                      double *time_copy, double *time_mpi, magma_queue_t queue) {
    double tic;
    int *lpmd = (int*)((void*)st_ctl->param + st_ctl->lpmd_offset); 
    int mpinr = lpmd[2]; 
    int nrank = lpmd[1]; 

    if (nrank > 1) {
        int *lsp = (int*)((void*)st_ctl->param + st_ctl->lsp_offset);
        int *lnp = (int*)((void*)st_ctl->param + st_ctl->lnp_offset);

        magma_queue_sync( queue );
        #if defined(PROF_MAGMA_BATCH)
        tic = MPI_Wtime();
        #endif
        if (flag == 1) {
            int ione = 1;
            double zero = 0.0;
            #if defined(USE_GDR)
            //magmablas_dlaset( MagmaFull, nd, ione, zero, zero, zau_gpu, nd, queue );
            #else
            lapackf77_dlaset( "F", &nd, &ione, &zero, &zero, zau, &nd );
            magma_dgetvector( lnp[mpinr], &zau_gpu[lsp[mpinr]-1], 1, &zau[lsp[mpinr]-1], 1, queue );
            #endif
        }
        #if defined(PROF_MAGMA_BATCH)
        *time_copy += MPI_Wtime()-tic;
        #endif

        #if defined(USE_GDR)
        c_hacapk_adot_cax_lfmtx_comm(zau_gpu, st_ctl,buffer,disps, wws,wwr, isct,irct, nd, queue, time_mpi);
        #else
        c_hacapk_adot_cax_lfmtx_comm(zau, st_ctl,buffer,disps, wws,wwr, isct,irct, nd, NULL, time_mpi);
        #endif

        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue );
        tic = MPI_Wtime();
        #endif
        #if !defined(USE_GDR)
        magma_dsetvector_async( nd, zau, 1, zau_gpu, 1, queue );
        #endif
        #if defined(PROF_MAGMA_BATCH)
        *time_copy += MPI_Wtime()-tic;
        #endif
    }
}


void c_hacapk_bicgstab_cax_lfmtx_flat_(stc_HACApK_leafmtxp *st_leafmtxp, stc_HACApK_lcontrol *st_ctl,
                                       double *u, double *b, double*param, int *nd, int *nstp, int *lrtrn) {
    // local constants
    int ione = 1;
    double zero =  0.0;
    double one  =  1.0;
    double mone = -1.0;
    // local arrays
    double *zr, *zshdw, *zp, *zt, *zkp, *zakp, *zkt, *zakt;
    double *wws, *wwr;
    int *lpmd = (int*)((void*)st_ctl->param + st_ctl->lpmd_offset);
    int isct[2], irct[2];
    // local variables
    double eps, alpha, beta, zeta, zz, zden, znorm, znormold, bnorm, zrnorm;
    double en_measure_time, st_measure_time, time;
    int info, step, mstep;
    int mpinr, nrank, ierr;
    double time_spmv, time_mpi, time_batch, time_set, time_copy, tic;
 
    MPI_Comm icomm = MPI_COMM_WORLD; //lpmd[0];
    mstep = param[82];
    eps = param[90];
    mpinr = lpmd[2]; 
    nrank = lpmd[1]; 
    MPI_Barrier( icomm );

    wws = (double*)malloc((*nd) * sizeof(double));
    wwr = (double*)malloc((*nd) * sizeof(double));

    zt = (double*)malloc((*nd) * sizeof(double));
    zr = (double*)malloc((*nd) * sizeof(double));
    zp = (double*)malloc((*nd) * sizeof(double));
    zkp = (double*)malloc((*nd) * sizeof(double));
    zakp = (double*)malloc((*nd) * sizeof(double));
    zkt = (double*)malloc((*nd) * sizeof(double));
    zakt= (double*)malloc((*nd) * sizeof(double));
    zshdw = (double*)malloc((*nd) * sizeof(double));

    // MPI buffer
    double * buffer = NULL;
    int *disps = NULL;
    c_hacapk_adot_cax_lfmtx_comm_setup(st_ctl, &buffer, &disps, NULL);

    // copy matrix to GPU
    c_hacapk_adot_body_lfcpy_batch_sorted_(nd, st_leafmtxp);
    //
    time_spmv = 0.0;
    time_mpi = 0.0;
    time_batch = 0.0;
    time_set = 0.0;
    time_copy = 0.0;
    MPI_Barrier( icomm );
    st_measure_time = MPI_Wtime();
    // init
    alpha = 0.0; beta = 0.0; zeta = 0.0;
    zz = c_hacapk_dotp_d(*nd, b, b ); 
    bnorm=sqrt(zz);
    lapackf77_dlacpy( "F", nd, &ione, b, nd, zr, nd );
    //  .. SpMV ..
    tic = MPI_Wtime();
    lapackf77_dlaset( "F", nd, &ione, &zero, &zero, zshdw, nd );
    c_hacapk_adot_body_lfmtx_batch_(zshdw,st_leafmtxp,u,wws, &time_batch,&time_set,&time_copy);
    time_spmv += (MPI_Wtime()-tic);
    c_hacapk_adot_cax_lfmtx_comm(zshdw, st_ctl,buffer,disps, wws, wwr, isct, irct, *nd, NULL, &time_mpi);
    //
    blasf77_daxpy( nd, &mone, zshdw, &ione, zr, &ione );
    lapackf77_dlacpy( "F", nd, &ione, zr, nd, zshdw, nd );
    zrnorm = c_hacapk_dotp_d(*nd, zr, zr ); 
    zrnorm = sqrt(zrnorm);
    if (mpinr == 0) {
        printf( "\n ** BICG (c version, flat) **\n" );
        printf( "\nOriginal relative residual norm = %.2e/%.2e = %.2e\n",zrnorm,bnorm,zrnorm/bnorm );
        printf( "HACApK_bicgstab_lfmtx_flat start\n" );
    }
    for ( step=1; step<=mstep; step++ ) {
        if (zrnorm/bnorm < eps) break;
        // zp(:nd) = zr(:nd) + beta*(zp(:nd) - zeta*zakp(:nd))
        if (beta == zero) {
            lapackf77_dlacpy( "F", nd, &ione, zr, nd, zp, nd );
        } else {
            blasf77_daxpy( nd, &zeta, zakp, &ione, zp, &ione );
            lapackf77_dlascl( "G", &ione, &ione, &one, &beta, nd, &ione, zp, nd, &info );
            blasf77_daxpy( nd, &one, zr, &ione, zp, &ione );
        }
        // zkp(:nd) = zp(:nd)
        lapackf77_dlacpy( "F", nd, &ione, zp, nd, zkp, nd );
        //  .. SpMV ..
        lapackf77_dlaset( "F", nd, &ione, &zero, &zero, zakp, nd );
        tic = MPI_Wtime();
        c_hacapk_adot_body_lfmtx_batch_(zakp,st_leafmtxp,zkp,wws, &time_batch,&time_set,&time_copy);
        time_spmv += (MPI_Wtime()-tic);
        c_hacapk_adot_cax_lfmtx_comm(zakp, st_ctl,buffer,disps, wws,wwr,isct,irct,*nd, NULL, &time_mpi);
        //
        znorm = c_hacapk_dotp_d(*nd, zshdw, zr ); 
        zden = c_hacapk_dotp_d(*nd, zshdw, zakp );
        alpha = -znorm/zden;
        znormold = znorm;
        // zt(:nd) = zr(:nd) - alpha*zakp(:nd)
        lapackf77_dlacpy( "F", nd, &ione, zr, nd, zt, nd );
        blasf77_daxpy( nd, &alpha, zakp, &ione, zt, &ione );
        alpha = -alpha;
        // zkt(:nd) = zt(:nd)
        lapackf77_dlacpy( "F", nd, &ione, zt, nd, zkt, nd );
        //  .. SpMV ..
        lapackf77_dlaset( "F", nd, &ione, &zero, &zero, zakt, nd );
        tic = MPI_Wtime();
        c_hacapk_adot_body_lfmtx_batch_(zakt,st_leafmtxp,zkt,wws, &time_batch,&time_set,&time_copy);
        time_spmv += (MPI_Wtime()-tic);
        c_hacapk_adot_cax_lfmtx_comm(zakt, st_ctl,buffer,disps, wws,wwr,isct,irct,*nd, NULL, &time_mpi);
        //
        znorm = c_hacapk_dotp_d(*nd, zakt, zt ); 
        zden = c_hacapk_dotp_d( *nd, zakt, zakt );
        zeta = znorm/zden;
        // u(:nd) = u(:nd) + alpha*zkp(:nd) + zeta*zkt(:nd)
        blasf77_daxpy( nd, &alpha, zkp, &ione, u, &ione );
        blasf77_daxpy( nd, &zeta,  zkt, &ione, u, &ione );
        // zr(:nd) = zt(:nd) - zeta*zakt(:nd)
        zeta = -zeta;
        lapackf77_dlacpy( "F", nd, &ione, zt, nd, zr, nd );
        blasf77_daxpy( nd, &zeta, zakt, &ione, zr, &ione );
        // beta = alpha/zeta * HACApK_dotp_d(nd,zshdw,zr)/znormold;
        beta = c_hacapk_dotp_d(*nd, zshdw, zr);
        beta = -alpha/zeta * beta/znormold;
        zrnorm = c_hacapk_dotp_d(*nd, zr, zr); 
        zrnorm = sqrt(zrnorm);
        *nstp = step;
        en_measure_time = MPI_Wtime();
        time = en_measure_time - st_measure_time;
        if (st_ctl->param[0] > 0 && mpinr == 0) {
            printf( " %d: time=%.2e log10(zrnorm/bnorm)=log10(%.2e/%.2e)=%.2e\n",step,time,zrnorm,bnorm,log10(zrnorm/bnorm) );
        }
    }
    MPI_Barrier( icomm );
    en_measure_time = MPI_Wtime();
    time = en_measure_time - st_measure_time;
    if (st_ctl->param[0] > 0) {
        //printf( " End: %d, %.2e\n",mpinr,time );
        if (mpinr == 0) {
            printf( "       BiCG        = %.5e\n", time );
            printf( "        time_mpi   = %.5e\n", time_mpi );
            printf( "        time_spmv  = %.5e\n", time_spmv );
            printf( "        > time_copy  = %.5e\n", time_copy );
            printf( "        > time_set   = %.5e\n", time_set );
            printf( "        > time_batch = %.5e\n", time_batch );
        }
    }
    // delete matrix
    c_hacapk_adot_body_lfdel_batch_(st_leafmtxp);

    // free cpu memory
    if (buffer != NULL) free(buffer);
    free(disps);
    free(wws);
    free(wwr);

    free(zt);
    free(zr);
    free(zp);
    free(zkp);
    free(zakp);
    free(zkt);
    free(zakt);
    free(zshdw);
}

#include "magma_v2.h"
// BATCH on GPU
void c_hacapk_bicgstab_cax_lfmtx_gpu_(stc_HACApK_leafmtxp *st_leafmtxp, stc_HACApK_lcontrol *st_ctl,
                                      double *u_cpu, double *b_cpu, double*param, int *nd, int *nstp, int *lrtrn) {
    // local constants
    int ione = 1;
    double zero =  0.0;
    double one  =  1.0;
    double mone = -1.0;
    // local arrays
    double *zr, *zshdw, *zp, *zt, *zkp, *zakp, *zkt, *zakt;
    double *b, *u, *wws, *wwr;
    double *wws_cpu, *wwr_cpu, *zau_cpu;
    double *dnorm, *hnorm;
    int *lpmd = (int*)((void*)st_ctl->param + st_ctl->lpmd_offset);
    int isct[2], irct[2];
    // local variables
    double eps, alpha, beta, zeta, zz, zden, znorm, znormold, bnorm, zrnorm;
    double en_measure_time, st_measure_time, time;
    int info, step, mstep;
    int mpinr, nrank, ierr;
    double time_spmv, time_mpi, time_batch, time_set, time_copy, time_dot, tic;
 
    MPI_Comm icomm = MPI_COMM_WORLD; //lpmd[0];
    mstep = param[82];
    eps = param[90];
    mpinr = lpmd[2]; 
    nrank = lpmd[1]; 
    MPI_Barrier( icomm );
    #ifdef GPU_AWARE
    if (mpinr == 0) {
        printf( "Compile time check : ");
        #if defined(MPIX_CUDA_AWARE_SUPPORT) && MPIX_CUDA_AWARE_SUPPORT
        printf( "This MPI library has CUDA-aware support.\n" );
        #elif defined(MPIX_CUDA_AWARE_SUPPORT) && !MPIX_CUDA_AWARE_SUPPORT
        printf( "This MPI library does not have CUDA-aware support.\n" );
        #else
        printf( "This MPI library cannot determine if there is CUDA-aware support.\n" );
        #endif /* MPIX_CUDA_AWARE_SUPPORT */
  
        printf( "Run time check     : " );
        #if defined(MPIX_CUDA_AWARE_SUPPORT)
        if (1 == MPIX_Query_cuda_support()) {
            printf( "This MPI library has CUDA-aware support.\n" );
        } else {
            printf( "This MPI library does not have CUDA-aware support.\n" );
        }
        #else /* !defined(MPIX_CUDA_AWARE_SUPPORT) */
        printf( "This MPI library cannot determine if there is CUDA-aware support.\n" );
        #endif /* MPIX_CUDA_AWARE_SUPPORT */
    }
    #endif

    int on_gpu = 1;
    magma_device_t cdev;
    magma_queue_t queue;

    magma_setdevice( get_device_id(st_leafmtxp) );
    magma_getdevice( &cdev );
    magma_queue_create( cdev, &queue );

    magma_queue_t *queue_hcmv = NULL;
    magma_event_t *event_hcmv = NULL;
    if (num_queues > 1) {
        int i;
        queue_hcmv = (magma_queue_t*)malloc(num_queues * sizeof(magma_queue_t));
        event_hcmv = (magma_event_t*)malloc(num_queues * sizeof(magma_event_t));
        for (i=0; i<num_queues; i++) {
            magma_queue_create( cdev, &queue_hcmv[i] );
            magma_event_create( &event_hcmv[i] );
        }
    }
    if (MAGMA_SUCCESS != magma_dmalloc(&u, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc(&b, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc(&zt,(*nd)*2) ||
        MAGMA_SUCCESS != magma_dmalloc(&zp, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc(&wws, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc(&wwr, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc(&zkp, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc(&zkt, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc(&zakp, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc(&zshdw,(*nd)*3) ||
        MAGMA_SUCCESS != magma_dmalloc(&dnorm, 2)) {
      printf( " failed to allocate u or b (nd=%d)\n",*nd );
    }
    zr   = &zshdw[*nd];
    zakp = &zr[*nd];
    zakt = &zt[*nd];
    // use pinned memory for buffer
    if (MAGMA_SUCCESS != magma_dmalloc_pinned(&wws_cpu, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc_pinned(&wwr_cpu, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc_pinned(&zau_cpu, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc_pinned(&hnorm, 2)) {
      printf( " failed to allocate pinned memory (nd=%d)\n",*nd );
    }

    // MPI buffer
    double *buffer = NULL;
    int *disps = NULL;
    c_hacapk_adot_cax_lfmtx_comm_setup(st_ctl, &buffer, &disps, queue);

    // copy matrix to GPU
    c_hacapk_adot_body_lfcpy_batch_sorted_(nd, st_leafmtxp);
    //
    time_spmv = 0.0;
    time_mpi = 0.0;
    time_batch = 0.0;
    time_set = 0.0;
    time_copy = 0.0;
    magma_queue_sync( queue );
    time_dot = 0.0;
    MPI_Barrier( icomm );
    st_measure_time = MPI_Wtime();
    // copy the input vector to GPU
    magma_dsetvector_async( *nd, b_cpu, 1, b, 1, queue );
    magma_dsetvector_async( *nd, u_cpu, 1, u, 1, queue );
    // init
    alpha = 0.0; beta = 0.0; zeta = 0.0;
    #if defined(PROF_MAGMA_BATCH)
    magma_queue_sync( queue );
    tic = MPI_Wtime();
    #endif
    #if defined(DDOT_BY_DGEMV)
    magma_dgemv( MagmaTrans, *nd, 1,
                 one,  b, *nd,
                       b, ione,
                 zero, dnorm, ione,
                 queue );
    magma_dgetvector( 1, dnorm, 1, hnorm, 1, queue );
    zz = hnorm[0];
    #else
    zz = magma_ddot(*nd, b, ione, b, ione, queue); 
    #endif
    #if defined(PROF_MAGMA_BATCH)
    magma_queue_sync( queue );
    time_dot += (MPI_Wtime()-tic);
    #endif
    bnorm=sqrt(zz);
    //  .. SpMV ..
    //magmablas_dlaset( MagmaFull, *nd, ione, zero, zero, zshdw, *nd, queue );
    #if defined(PROF_MAGMA_BATCH)
    magma_queue_sync( queue );
    tic = MPI_Wtime();
    #endif
    c_hacapk_adot_body_lfmtx_batch_queue(zshdw,st_leafmtxp,u,wws, &time_batch,&time_set,&time_copy, 
                                         on_gpu, queue, queue_hcmv, event_hcmv);
    #if defined(PROF_MAGMA_BATCH)
    magma_queue_sync( queue );
    time_spmv += (MPI_Wtime()-tic);
    #endif
    c_hacapk_adot_cax_lfmtx_comm_gpu(1, zshdw, zau_cpu, st_ctl,buffer,disps, wws_cpu, wwr_cpu, isct, irct, *nd, 
                                     &time_copy,&time_mpi, queue);
    //
    magmablas_dlacpy( MagmaFull, *nd, ione, b, *nd, zr, *nd, queue );
    magma_daxpy( *nd, mone, zshdw, ione, zr, ione, queue );
    magmablas_dlacpy( MagmaFull, *nd, ione, zr, *nd, zshdw, *nd, queue );
    #if defined(PROF_MAGMA_BATCH)
    magma_queue_sync( queue );
    tic = MPI_Wtime();
    #endif
    #if defined(DDOT_BY_DGEMV)
    magma_dgemv( MagmaTrans, *nd, 1,
                 one,  zr, *nd,
                       zr, ione,
                 zero, dnorm, ione,
                 queue );
    magma_dgetvector( 1, dnorm, 1, hnorm, 1, queue );
    zrnorm = hnorm[0];
    #else
    zrnorm = magma_ddot(*nd, zr, ione, zr, ione, queue); 
    #endif
    #if defined(PROF_MAGMA_BATCH)
    magma_queue_sync( queue );
    time_dot += (MPI_Wtime()-tic);
    #endif
    zrnorm = sqrt(zrnorm);
    if (mpinr == 0) {
        printf( "\n ** BICG with MAGMA batched on GPU **\n" );
        printf( "\nOriginal relative residual norm = %.2e/%.2e = %.2e\n",zrnorm,bnorm,zrnorm/bnorm );
        printf( " first time_mpi=%.2e\n",time_mpi );
        printf( "HACApK_bicgstab_lfmtx_gpu start\n" );
        if (num_queues > 1) printf( " ** num_queues = %d **\n",num_queues );
    }
    for ( step=1; step<=mstep; step++ ) {
        if (zrnorm/bnorm < eps) break;
        // zp(:nd) = zr(:nd) + beta*(zp(:nd) - zeta*zakp(:nd))
        if (beta == zero) {
            magmablas_dlacpy( MagmaFull, *nd, ione, zr, *nd, zp, *nd, queue );
        } else {
            magma_daxpy( *nd, zeta, zakp, ione, zp, ione, queue );
            magmablas_dlascl( MagmaFull, ione, ione, one, beta, *nd, ione, zp, *nd, queue, &info );
            magma_daxpy( *nd, one, zr, ione, zp, ione, queue );
        }
        // zkp(:nd) = zp(:nd)
        magmablas_dlacpy( MagmaFull, *nd, ione, zp, *nd, zkp, *nd, queue );
        //  .. SpMV ..
        //magmablas_dlaset( MagmaFull, *nd, ione, zero, zero, zakp, *nd, queue );
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue );
        tic = MPI_Wtime();
        #endif
        c_hacapk_adot_body_lfmtx_batch_queue(zakp,st_leafmtxp,zkp,wws, &time_batch,&time_set,&time_copy,
                                             on_gpu, queue, queue_hcmv, event_hcmv);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue );
        time_spmv += (MPI_Wtime()-tic);
        #endif
        c_hacapk_adot_cax_lfmtx_comm_gpu(1, zakp, zau_cpu, st_ctl,buffer,disps, wws_cpu,wwr_cpu, isct,irct,*nd, 
                                         &time_copy,&time_mpi, queue);
        //
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue );
        tic = MPI_Wtime();
        #endif
        #if defined(DDOT_BY_DGEMV)
        magma_dgemv( MagmaTrans, *nd, 2,
                     one,  zr, *nd,
                           zshdw, ione,
                     zero, dnorm, ione,
                     queue );
        magma_dgetvector( 2, dnorm, 1, hnorm, 1, queue );
        znorm = hnorm[0];
        zden  = hnorm[1];
        #else
        znorm = magma_ddot(*nd, zshdw, ione, zr, ione, queue); 
        zden  = magma_ddot(*nd, zshdw, ione, zakp, ione, queue);
        #endif
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue );
        time_dot += (MPI_Wtime()-tic);
        #endif
        alpha = -znorm/zden;
        znormold = znorm;
        // zt(:nd) = zr(:nd) - alpha*zakp(:nd)
        magmablas_dlacpy( MagmaFull, *nd, ione, zr, *nd, zt, *nd, queue );
        magma_daxpy( *nd, alpha, zakp, ione, zt, ione, queue );
        alpha = -alpha;
        // zkt(:nd) = zt(:nd)
        magmablas_dlacpy( MagmaFull, *nd, ione, zt, *nd, zkt, *nd, queue );
        //  .. SpMV ..
        //magmablas_dlaset( MagmaFull, *nd, ione, zero, zero, zakt, *nd, queue );
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue );
        tic = MPI_Wtime();
        #endif
        c_hacapk_adot_body_lfmtx_batch_queue(zakt,st_leafmtxp,zkt,wws, &time_batch,&time_set,&time_copy,
                                             on_gpu, queue, queue_hcmv, event_hcmv);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue );
        time_spmv += (MPI_Wtime()-tic);
        #endif
        c_hacapk_adot_cax_lfmtx_comm_gpu(1, zakt,zau_cpu, st_ctl,buffer,disps, wws_cpu,wwr_cpu, isct,irct,*nd, 
                                         &time_copy,&time_mpi, queue);
        //
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue );
        tic = MPI_Wtime();
        #endif
        #if defined(DDOT_BY_DGEMV)
        magma_dgemv( MagmaTrans, *nd, 2,
                     one,  zt, *nd,
                           zakt, ione,
                     zero, dnorm, ione,
                     queue );
        magma_dgetvector( 2, dnorm, 1, hnorm, 1, queue );
        znorm = hnorm[0];
        zden  = hnorm[1];
        #else
        znorm = magma_ddot(*nd, zakt, ione, zt, ione, queue); 
        zden  = magma_ddot(*nd, zakt, ione, zakt, ione, queue);
        #endif
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue );
        time_dot += (MPI_Wtime()-tic);
        #endif
        zeta = znorm/zden;
        // u(:nd) = u(:nd) + alpha*zkp(:nd) + zeta*zkt(:nd)
        magma_daxpy( *nd, alpha, zkp, ione, u, ione, queue );
        magma_daxpy( *nd, zeta,  zkt, ione, u, ione, queue );
        // zr(:nd) = zt(:nd) - zeta*zakt(:nd)
        zeta = -zeta;
        magmablas_dlacpy( MagmaFull, *nd, ione, zt, *nd, zr, *nd, queue );
        magma_daxpy( *nd, zeta, zakt, ione, zr, ione, queue );
        // beta = alpha/zeta * HACApK_dotp_d(nd,zshdw,zr)/znormold;
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue );
        tic = MPI_Wtime();
        #endif
        #if defined(DDOT_BY_DGEMV)
        magma_dgemv( MagmaTrans, *nd, 2,
                     one,  zshdw, *nd,
                           zr, ione,
                     zero, dnorm, ione,
                     queue );
        magma_dgetvector( 2, dnorm, 1, hnorm, 1, queue );
        beta   = hnorm[0];
        zrnorm = hnorm[1];
        #else
        beta   = magma_ddot(*nd, zshdw, ione, zr, ione, queue);
        zrnorm = magma_ddot(*nd, zr,    ione, zr, ione, queue);
        #endif
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue );
        time_dot += (MPI_Wtime()-tic);
        #endif
        beta = -alpha/zeta * beta/znormold;
        zrnorm = sqrt(zrnorm);
        *nstp = step;
        en_measure_time = MPI_Wtime();
        time = en_measure_time - st_measure_time;
        if (st_ctl->param[0] > 0 && mpinr == 0) {
            printf( " %d: time=%.2e log10(zrnorm/bnorm)=log10(%.2e/%.2e)=%.2e\n",step,time,zrnorm,bnorm,log10(zrnorm/bnorm) );
        }
    }
    magma_dgetvector( *nd, u, 1, u_cpu, 1, queue );
    MPI_Barrier( icomm );
    en_measure_time = MPI_Wtime();
    time = en_measure_time - st_measure_time;
#if 1
    c_hacapk_adot_body_lfmtx_batch_queue(zshdw,st_leafmtxp,u,wws, &time_batch,&time_set,&time_copy, 
                                         on_gpu, queue, NULL, NULL);
    c_hacapk_adot_cax_lfmtx_comm_gpu(1, zshdw, zau_cpu, st_ctl,buffer,disps, wws_cpu, wwr_cpu, isct, irct, *nd, 
                                     &time_copy,&time_mpi, queue);
    magmablas_dlascl( MagmaFull, ione, ione, one, mone, *nd, ione, zshdw, *nd, queue, &info );
    magma_daxpy( *nd, one, b, ione, zshdw, ione, queue );
    zrnorm = sqrt(magma_ddot(*nd, zshdw, ione, zshdw, ione, queue));
    printf( " resnorm=%.2e\n",zrnorm );
#endif
    if (st_ctl->param[0] > 0) {
        st_leafmtxp->gflops *= (1.0+2.0*(double)step);
        //printf( " End: %d, %.2e\n",mpinr,time );
        if (mpinr == 0) {
            printf( "       BiCG        = %.5e\n", time );
            printf( "        time_mpi   = %.5e\n", time_mpi );
            printf( "        time_copy  = %.5e\n", time_copy );
            printf( "        time_spmv  = %.5e\n", time_spmv );
            printf( "        > time_batch = %.5e (%.2fGflop/s)\n", time_batch,
                    st_leafmtxp->gflops/time_batch );
            printf( "        > time_set   = %.5e\n", time_set );
            printf( "        time_dott  = %.5e\n", time_dot );
        }
        FILE *fp;
        char filename[100];
        sprintf(filename,"summary_%d.dat",mpinr);
        fp = fopen(filename,"w");
        fprintf( fp,"       BiCG        = %.5e\n", time );
        fprintf( fp,"        time_mpi   = %.5e\n", time_mpi );
        fprintf( fp,"        time_copy  = %.5e\n", time_copy );
        fprintf( fp,"        time_spmv  = %.5e\n", time_spmv );
        fprintf( fp,"        > time_batch = %.5e (%.2fGflop, %.2fGflop/s)\n", time_batch,
                 st_leafmtxp->gflops,st_leafmtxp->gflops/time_batch );
        fprintf( fp,"        > time_set   = %.5e\n", time_set );
        fclose(fp);
    }
    magma_queue_sync( queue );
    magma_queue_destroy( queue );

    // delete matrix
    c_hacapk_adot_body_lfdel_batch_(st_leafmtxp);

    // free gpu memory
    magma_free(u);
    magma_free(b);

    magma_free(wws);
    magma_free(wwr);

    magma_free(zt);
    magma_free(zr);
    magma_free(zp);
    magma_free(zkp);
    magma_free(zkt);
    magma_free(zshdw);

    // free cpu memory
    if (buffer != NULL) {
        #if defined(USE_GDR)
        magma_free(buffer);
        #else
        free(buffer);
        #endif
    }
    free(disps);
    //free(wws_cpu);
    //free(wwr_cpu);
    magma_free_pinned(wws_cpu);
    magma_free_pinned(wwr_cpu);
    magma_free_pinned(zau_cpu);
}


#define ACCUM_ON_CPU
#define DDOT_REDUCE_ON_CPU

//////////////////////////////////////////////////////////////////////
// BICG on multiple GPUs (redudant vector operation on each GPU)
void c_hacapk_bicgstab_cax_lfmtx_mgpu1_(stc_HACApK_leafmtxp *st_leafmtxp, stc_HACApK_lcontrol *st_ctl,
                                        double *u_cpu, double *b_cpu, double*param, int *nd, int *nstp, int *lrtrn) {
    // local constants
    int ione = 1;
    double zero =  0.0;
    double one  =  1.0;
    double mone = -1.0;
    // local arrays
    int *lpmd = (int*)((void*)st_ctl->param + st_ctl->lpmd_offset);
    int isct[2], irct[2];
    // local variables
    double eps, alpha, beta, zeta, zz, zden, znorm, znormold, bnorm, zrnorm;
    double en_measure_time, st_measure_time, time;
    int info, step, mstep;
    int mpinr, nrank, ierr;
    double time_spmv, time_mpi, time_batch, time_set, time_copy, time_dot, tic, toc;
    double time_set1, time_set2, time_set3, time_set4; 

    MPI_Comm icomm = MPI_COMM_WORLD; //lpmd[0];
    mstep = param[82];
    eps = param[90];
    mpinr = lpmd[2]; 
    nrank = lpmd[1]; 
    MPI_Barrier( icomm );
    #ifdef GPU_AWARE
    if (mpinr == 0) {
        printf( "Compile time check : ");
        #if defined(MPIX_CUDA_AWARE_SUPPORT) && MPIX_CUDA_AWARE_SUPPORT
        printf( "This MPI library has CUDA-aware support.\n" );
        #elif defined(MPIX_CUDA_AWARE_SUPPORT) && !MPIX_CUDA_AWARE_SUPPORT
        printf( "This MPI library does not have CUDA-aware support.\n" );
        #else
        printf( "This MPI library cannot determine if there is CUDA-aware support.\n" );
        #endif /* MPIX_CUDA_AWARE_SUPPORT */
  
        printf( "Run time check     : " );
        #if defined(MPIX_CUDA_AWARE_SUPPORT)
        if (1 == MPIX_Query_cuda_support()) {
            printf( "This MPI library has CUDA-aware support.\n" );
        } else {
            printf( "This MPI library does not have CUDA-aware support.\n" );
        }
        #else /* !defined(MPIX_CUDA_AWARE_SUPPORT) */
        printf( "This MPI library cannot determine if there is CUDA-aware support.\n" );
        #endif /* MPIX_CUDA_AWARE_SUPPORT */
    }
    #endif


    //int gpus_per_node = 4;
    //magma_device_t devices[ MagmaMaxGPUs ];
    //magma_getdevices( devices, MagmaMaxGPUs, &gpus_per_node );

    #if defined(ACCUM_ON_CPU)
    int flag = 0;
    #else
    int flag = 1;
    #endif
    int on_gpu = 1, d, gpu_id = (gpus_per_proc*get_device_id(st_leafmtxp))%gpus_per_node;
    printf( " process %d on GPU(%d:%d)\n",mpinr,gpu_id,gpu_id+gpus_per_proc-1);

    magma_device_t cdev;
    magma_queue_t *queue = (magma_queue_t *)malloc(2*gpus_per_proc * sizeof(magma_queue_t));
    magma_event_t *event = (magma_event_t *)malloc(  gpus_per_proc * sizeof(magma_event_t));
    magma_queue_t **queue_hcmv = NULL;
    magma_event_t **event_hcmv = NULL;
    if (num_queues > 1) {
        queue_hcmv = (magma_queue_t**)malloc(gpus_per_proc * sizeof(magma_queue_t**));
        event_hcmv = (magma_event_t**)malloc(gpus_per_proc * sizeof(magma_event_t**));
    }
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_getdevice( &cdev );
        magma_queue_create( cdev, &queue[d] );
        magma_queue_create( cdev, &queue[d+gpus_per_proc] );
        magma_event_create( &event[d] );
        if (num_queues > 1) {
            int i;
            queue_hcmv[d] = (magma_queue_t*)malloc(   num_queues  * sizeof(magma_queue_t));
            event_hcmv[d] = (magma_event_t*)malloc((1+num_queues) * sizeof(magma_event_t));
            for (i=0; i<num_queues; i++) {
                magma_queue_create( cdev, &queue_hcmv[d][i] );
                magma_event_create( &event_hcmv[d][i] );
            }
            magma_event_create( &event_hcmv[d][num_queues] );
        }
    }
    // main GPU
    magma_setdevice(gpu_id);
    st_leafmtxp->iwork = (int*)malloc(2*gpus_per_proc * sizeof(int));

    // use pinned memory for CPU buffer
    double *zau_cpu1, *zau_cpu2, *wws_cpu, *wwr_cpu;
    magma_dmalloc_pinned(&zau_cpu1, *nd);
    magma_dmalloc_pinned(&zau_cpu2, *nd);
    magma_dmalloc_pinned(&wws_cpu,  *nd);
    magma_dmalloc_pinned(&wwr_cpu, (*nd)*gpus_per_proc);

    // allocate GPU vectors
    double *wws, *wwr;
    if (MAGMA_SUCCESS != magma_dmalloc(&wws, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc(&wwr, *nd)) {
      printf( " failed to allocate vectors (nd=%d)\n",*nd );
    }

    double **b = (double**)malloc(gpus_per_proc * sizeof(double*));
#define redundant_u
#if !defined(redundant_u)
    double **u = (double**)malloc(gpus_per_proc * sizeof(double*));
#endif
    double **zr = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zp = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zt = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zkp = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zkt = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zakp =(double**)malloc(gpus_per_proc * sizeof(double*));
    double **zshdw = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zakt = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **dBuffer = (double**)malloc(gpus_per_proc * sizeof(double*));
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        if (MAGMA_SUCCESS != magma_dmalloc(&b[d], *nd) ||
#if !defined(redundant_u)
            MAGMA_SUCCESS != magma_dmalloc(&u[d], *nd) ||
#endif
            MAGMA_SUCCESS != magma_dmalloc(&zt[d], 2*(*nd)) ||
            MAGMA_SUCCESS != magma_dmalloc(&zp[d], *nd) ||
            MAGMA_SUCCESS != magma_dmalloc(&zkp[d], *nd) ||
            MAGMA_SUCCESS != magma_dmalloc(&zkt[d], *nd) ||
            MAGMA_SUCCESS != magma_dmalloc(&zshdw[d], 3*(*nd))) {
          printf( " failed to allocate vectors (nd=%d)\n",*nd );
        }
        zr[d]   = &zshdw[d][*nd];
        zakp[d] = &zr[d][*nd];
        zakt[d] = &zt[d][*nd];
    }
    double *dnorm, *hnorm;
    magma_setdevice(gpu_id);
    magma_dmalloc(&dnorm, 2);
    for (d=0; d<gpus_per_proc; d++) {
        magma_dmalloc(&dBuffer[d], *nd);
    }
    magma_dmalloc_pinned(&hnorm, 2);
    // setup peers
    for (d=1; d<gpus_per_proc; d++) {
        int canAccessPeer;
        magma_setdevice(gpu_id+d);
        cudaDeviceCanAccessPeer(&canAccessPeer, gpu_id+d, gpu_id);
        if (canAccessPeer == 1) {
            if (cudaDeviceEnablePeerAccess( gpu_id, 0 ) != cudaSuccess) {
                printf( " %d:%d: cudaDeviceEnablePeerAccess( %d ) failed\n",mpinr,gpu_id+d,gpu_id );
            }
        } else {
            printf( "cudaDeviceCanAccessPeer( %d, %d ) failed\n",gpu_id+d,gpu_id );
        }
    }
    // MPI buffer
    double *buffer = NULL;
    int *disps = NULL;
    c_hacapk_adot_cax_lfmtx_comm_setup(st_ctl, &buffer, &disps, NULL);

    #ifdef WARMUP_MPI
    c_hacapk_adot_cax_lfmtx_warmup(st_ctl, zau_cpu, wws_cpu, wwr_cpu, *nd);
    #endif
    // copy matrix to GPU
    c_hacapk_adot_body_lfcpy_batch_sorted_mgpu_(nd, st_leafmtxp, queue);
#if defined(redundant_u)
    double **u = st_leafmtxp->zu_mgpu;
#endif
    //
    time_spmv = 0.0;
    time_mpi = 0.0;
    time_batch = 0.0;
    time_set = 0.0;
    time_set1 = 0.0;
    time_set2 = 0.0;
    time_set3 = 0.0;
    time_set4 = 0.0;
    time_copy = 0.0;
    time_dot = 0.0;
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_queue_sync( queue[d] );
    }
    MPI_Barrier( icomm );
    st_measure_time = MPI_Wtime();
    // copy the input vector to GPU
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_dsetvector_async( *nd, b_cpu, 1, b[d], 1, queue[d] );
        magma_dsetvector_async( *nd, u_cpu, 1, u[d], 1, queue[d] );
    }
    // init
    alpha = 0.0; beta = 0.0; zeta = 0.0;
    magma_setdevice(gpu_id);
    zz = magma_ddot(*nd, b[0], ione, b[0], ione, queue[0]); 
    bnorm=sqrt(zz);
    // .. SpMV ..
    int flag_set = 1;
    #if defined(PROF_MAGMA_BATCH)
    magma_queue_sync( queue[0] );
    tic = MPI_Wtime();
    #endif
    c_hacapk_adot_body_lfmtx_batch_mgpu2(0, zshdw[0],st_leafmtxp,st_ctl,u,wws, zau_cpu1,wwr_cpu,
                                         dBuffer, event, &time_batch,&time_set,&time_copy,
                                         &time_set1, &time_set2, &time_set3,
                                         on_gpu, queue, queue_hcmv, event_hcmv);
    #if defined(PROF_MAGMA_BATCH)
    magma_queue_sync( queue[0] );
    time_spmv += (MPI_Wtime()-tic);
    #endif
    c_hacapk_adot_cax_lfmtx_comm_gpu(flag, zshdw[0], zau_cpu1,
                                     st_ctl,buffer,disps, wws_cpu, wwr_cpu, isct, irct, *nd, 
                                     &time_copy,&time_mpi, queue[0]);
    #if defined(PROF_MAGMA_BATCH)
    tic = MPI_Wtime();
    #endif
    #if 0 
    if (nrank == 1) {
        magma_setdevice(gpu_id);
        magma_dgetvector( *nd, zshdw[0], 1, zau_cpu1, 1, queue[0] );
    }
    for (d=1; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_dsetvector_async( *nd, zau_cpu1, 1, zshdw[d], 1, queue[d] );
    }
    #else
    magma_event_record( event[0], queue[0] );
    for (d=1; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_queue_wait_event( queue[d], event[0] );
        cudaMemcpyPeerAsync(zshdw[d], gpu_id+d, zshdw[0], gpu_id, (*nd)*sizeof(double),
                            magma_queue_get_cuda_stream( queue[d] ) );
        magma_event_record( event[d], queue[d] );
        magma_queue_wait_event( queue[0], event[d] );
    }
    #endif
    #if defined(PROF_MAGMA_BATCH)
    for (d=1; d<gpus_per_proc; d++) {
        magma_queue_sync( queue[d] );
    }
    toc = (MPI_Wtime()-tic);
    time_set  += toc;
    time_set4 += toc;
    #endif
    //
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magmablas_dlacpy( MagmaFull, *nd, ione, b[d], *nd, zr[d], *nd, queue[d] );
        magma_daxpy( *nd, mone, zshdw[d], ione, zr[d], ione, queue[d] );
        magmablas_dlacpy( MagmaFull, *nd, ione, zr[d], *nd, zshdw[d], *nd, queue[d] );
    }
    magma_setdevice(gpu_id);
    zrnorm = magma_ddot(*nd, zr[0], ione, zr[0], ione, queue[0]); 
    zrnorm = sqrt(zrnorm);
    if (mpinr == 0) {
        printf( "\n ** BICG with MAGMA batched on multiple GPUs (version 1, %d GPUs) **\n",gpus_per_proc );
        printf( "\nOriginal relative residual norm = %.2e/%.2e = %.2e\n",zrnorm,bnorm,zrnorm/bnorm );
        printf( " first time_mpi=%.2e\n",time_mpi );
        printf( "HACApK_bicgstab_lfmtx_mgpu2 start\n" );
        if (num_queues > 1) printf( " ** num_queues = %d **\n",num_queues );
    }
    for ( step=1; step<=mstep; step++ ) {
        if (zrnorm/bnorm < eps) break;
        // zp(:nd) = zr(:nd) + beta*(zp(:nd) - zeta*zakp(:nd))
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            if (beta == zero) {
                magmablas_dlacpy( MagmaFull, *nd, ione, zr[d], *nd, zp[d], *nd, queue[d] );
            } else {
                magma_daxpy( *nd, zeta, zakp[d], ione, zp[d], ione, queue[d] );
                magmablas_dlascl( MagmaFull, ione, ione, one, beta, *nd, ione, zp[d], *nd, queue[d], &info );
                magma_daxpy( *nd, one, zr[d], ione, zp[d], ione, queue[d] );
            }
        }
        // zkp(:nd) = zp(:nd)
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magmablas_dlacpy( MagmaFull, *nd, ione, zp[d], *nd, zkp[d], *nd, queue[d] );
        }
        //  .. SpMV ..
        magma_setdevice(gpu_id);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue[0] );
        tic = MPI_Wtime();
        #endif
        c_hacapk_adot_body_lfmtx_batch_mgpu2(flag_set, zakp[0],st_leafmtxp,st_ctl, zkp,wws, zau_cpu2,wwr_cpu,
                                             dBuffer,event, &time_batch,&time_set,&time_copy, 
                                             &time_set1, &time_set2, &time_set3,
                                             on_gpu, queue, queue_hcmv, event_hcmv);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue[0] );
        time_spmv += (MPI_Wtime()-tic);
        #endif
        c_hacapk_adot_cax_lfmtx_comm_gpu(flag, zakp[0], zau_cpu2,
                                         st_ctl,buffer,disps, wws_cpu,wwr_cpu, isct,irct,*nd, 
                                         &time_copy,&time_mpi, queue[0]);
        #if defined(PROF_MAGMA_BATCH)
        tic = MPI_Wtime();
        #endif
        #if 0
        if (nrank == 1) {
            magma_setdevice(gpu_id);
            magma_dgetvector( *nd, zakp[0], 1, zau_cpu2, 1, queue[0] );
        }
        for (d=1; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_dsetvector_async( *nd, zau_cpu2, 1, zakp[d], 1, queue[d] );
        }
        #else
        magma_event_record( event[0], queue[0] );
        for (d=1; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_queue_wait_event( queue[d], event[0] );
            cudaMemcpyPeerAsync(zakp[d], gpu_id+d, zakp[0], gpu_id, (*nd)*sizeof(double),
                                 magma_queue_get_cuda_stream( queue[d] ) );
            magma_event_record( event[d], queue[d] );
            magma_queue_wait_event( queue[0], event[d] );
        }
        #endif
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        toc = (MPI_Wtime()-tic);
        time_set  += toc;
        time_set4 += toc;

        tic = MPI_Wtime();
        #endif
        //
        magma_setdevice(gpu_id);
        #if defined(DDOT_BY_DGEMV)
        /*magmablas_dgemv( MagmaTrans, *nd, 2,
                         one,  zr[0], *nd,
                               zshdw[0], ione,
                         zero, dnorm, ione,
                         queue[0] );*/
        magma_dgemv( MagmaTrans, *nd, 2,
                     one,  zr[0], *nd,
                           zshdw[0], ione,
                     zero, dnorm, ione,
                     queue[0] );
        magma_dgetvector( 2, dnorm, 1, hnorm, 1, queue[0] );
        znorm = hnorm[0];
        zden  = hnorm[1];
        #else
        znorm = magma_ddot( *nd, zshdw[0], ione, zr[0], ione, queue[0] ); 
        zden  = magma_ddot( *nd, zshdw[0], ione, zakp[0], ione, queue[0] );
        #endif
        alpha = -znorm/zden;
        znormold = znorm;
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        time_dot += (MPI_Wtime()-tic);
        #endif
        // zt(:nd) = zr(:nd) - alpha*zakp(:nd)
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magmablas_dlacpy( MagmaFull, *nd, ione, zr[d], *nd, zt[d], *nd, queue[d] );
            magma_daxpy( *nd, alpha, zakp[d], ione, zt[d], ione, queue[d] );
        }
        alpha = -alpha;
        // zkt(:nd) = zt(:nd)
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magmablas_dlacpy( MagmaFull, *nd, ione, zt[d], *nd, zkt[d], *nd, queue[d] );
        }
        //  .. SpMV ..
        magma_setdevice(gpu_id);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue[0] );
        tic = MPI_Wtime();
        #endif
        c_hacapk_adot_body_lfmtx_batch_mgpu2(flag_set, zakt[0],st_leafmtxp,st_ctl, zkt,wws, zau_cpu1,wwr_cpu, 
                                             dBuffer,event, &time_batch,&time_set,&time_copy,
                                             &time_set1, &time_set2, &time_set3,
                                             on_gpu, queue, queue_hcmv, event_hcmv);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue[0] );
        time_spmv += (MPI_Wtime()-tic);
        #endif
        c_hacapk_adot_cax_lfmtx_comm_gpu(flag, zakt[0],zau_cpu1,
                                         st_ctl,buffer,disps, wws_cpu,wwr_cpu, isct,irct,*nd, 
                                         &time_copy,&time_mpi, queue[0]);
        #if defined(PROF_MAGMA_BATCH)
        tic = MPI_Wtime();
        #endif
        #if 0
        if (nrank == 1) {
            magma_setdevice(gpu_id);
            magma_dgetvector( *nd, zakt[0], 1, zau_cpu1, 1, queue[0] );
        }
        for (d=1; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_dsetvector_async( *nd, zau_cpu1, 1, zakt[d], 1, queue[d] );
        }
        #else
        magma_event_record( event[0], queue[0] );
        for (d=1; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_queue_wait_event( queue[d], event[0] );
            cudaMemcpyPeerAsync(zakt[d], gpu_id+d, zakt[0], gpu_id, (*nd)*sizeof(double),
                                 magma_queue_get_cuda_stream( queue[d] ) );
            magma_event_record( event[d], queue[d] );
            magma_queue_wait_event( queue[0], event[d] );
        }
        #endif
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        toc = (MPI_Wtime()-tic);
        time_set  += toc;
        time_set4 += toc;

        tic = MPI_Wtime();
        #endif
        //
        magma_setdevice(gpu_id);
        #if defined(DDOT_BY_DGEMV)
        /*magmablas_dgemv( MagmaTrans, *nd, 2,
                         one,  zt[0], *nd,
                               zakt[0], ione,
                         zero, dnorm, ione,
                         queue[0] );*/
        magma_dgemv( MagmaTrans, *nd, 2,
                     one,  zt[0], *nd,
                           zakt[0], ione,
                     zero, dnorm, ione,
                     queue[0] );
        magma_dgetvector( 2, dnorm, 1, hnorm, 1, queue[0] );
        znorm = hnorm[0];
        zden  = hnorm[1];
        #else
        znorm = magma_ddot( *nd, zakt[0], ione, zt[0], ione, queue[0] ); 
        zden  = magma_ddot( *nd, zakt[0], ione, zakt[0], ione, queue[0] );
        #endif
        zeta = znorm/zden;
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        time_dot += (MPI_Wtime()-tic);
        #endif
        // u(:nd) = u(:nd) + alpha*zkp(:nd) + zeta*zkt(:nd)
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_daxpy( *nd, alpha, zkp[d], ione, u[d], ione, queue[d] );
            magma_daxpy( *nd, zeta,  zkt[d], ione, u[d], ione, queue[d] );
        }
        // zr(:nd) = zt(:nd) - zeta*zakt(:nd)
        zeta = -zeta;
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magmablas_dlacpy( MagmaFull, *nd, ione, zt[d], *nd, zr[d], *nd, queue[d] );
            magma_daxpy( *nd, zeta, zakt[d], ione, zr[d], ione, queue[d] );
        }
        // beta = alpha/zeta * HACApK_dotp_d(nd,zshdw,zr)/znormold;
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        tic = MPI_Wtime();
        #endif
        magma_setdevice(gpu_id);
        #if defined(DDOT_BY_DGEMV)
        /*magmablas_dgemv( MagmaTrans, *nd, 2,
                         one,  zshdw[0], *nd,
                               zr[0], ione,
                         zero, dnorm, ione,
                         queue[0] );*/
        magma_dgemv( MagmaTrans, *nd, 2,
                     one,  zshdw[0], *nd,
                           zr[0], ione,
                     zero, dnorm, ione,
                     queue[0] );
        magma_dgetvector( 2, dnorm, 1, hnorm, 1, queue[0] );
        beta    = hnorm[0];
        zrnorm  = hnorm[1];
        #else
        beta   = magma_ddot( *nd, zshdw[0], ione, zr[0], ione, queue[0]);
        zrnorm = magma_ddot( *nd, zr[0],    ione, zr[0], ione, queue[0] );
        #endif
        beta = -alpha/zeta * beta/znormold;
        zrnorm = sqrt(zrnorm);
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        time_dot += (MPI_Wtime()-tic);
        #endif
        *nstp = step;
        en_measure_time = MPI_Wtime();
        time = en_measure_time - st_measure_time;
        if (st_ctl->param[0] > 0 && mpinr == 0) {
            printf( " %d: time=%.2e log10(zrnorm/bnorm)=log10(%.2e/%.2e)=%.2e\n",step,time,zrnorm,bnorm,log10(zrnorm/bnorm) );
        }
    }
    magma_dgetvector( *nd, u[0], 1, u_cpu, 1, queue[0] );
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_queue_sync( queue[d] );
    }
    MPI_Barrier( icomm );
    en_measure_time = MPI_Wtime();
    time = en_measure_time - st_measure_time;
    if (st_ctl->param[0]>0) {
        //printf( " End: %d, %.2e\n",mpinr,time );
        if (mpinr == 0) {
            printf( "       BiCG       = %.5e\n", time );
            printf( "        time_mpi  = %.5e\n", time_mpi );
            printf( "        time_copy = %.5e\n", time_copy );
            printf( "        time_spmv = %.5e\n", time_spmv );
            printf( "        > time_batch = %.5e\n", time_batch );
            printf( "        > time_set   = %.5e\n", time_set );
            printf( "          + time_set1 = %.5e\n", time_set1 );
            printf( "          + time_set2 = %.5e\n", time_set2 );
            printf( "          + time_set3 = %.5e\n", time_set3 );
            printf( "          + time_set4 = %.5e\n", time_set4 );
            printf( "          +          += %.5e\n", time_set1+time_set2+time_set3+time_set4 );
            printf( "        > time_dot   = %.5e\n", time_dot );
            printf( "                     => %.5e\n", time_mpi+time_copy+time_spmv+time_dot );
        }
    }

    // free gpu memory
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_queue_sync( queue[d] );
        magma_queue_destroy( queue[d] );
        magma_queue_destroy( queue[d+gpus_per_proc] );

#if !defined(redundant_u)
        magma_free(u[d]);
#endif
        magma_free(b[d]);

        magma_free(zt[d]);
        magma_free(zp[d]);
        magma_free(zkp[d]);
        magma_free(zkt[d]);
        magma_free(zshdw[d]);
    }
    // delete matrix
    c_hacapk_adot_body_lfdel_mgpu_(st_leafmtxp);

    magma_setdevice(gpu_id);
    // free cpu memory
    free(queue);
    if (buffer != NULL) free(buffer);
    free(disps);
#if !defined(redundant_u)
    free(u);
#endif
    free(b);
    free(zt);
    free(zr);
    free(zp);
    free(zkp);
    free(zakp);
    free(zkt);
    free(zakt);
    free(zshdw);

    magma_free_pinned(zau_cpu1);
    magma_free_pinned(zau_cpu2);
    magma_free_pinned(wws_cpu);
    magma_free_pinned(wwr_cpu);

    magma_free(wws);
    magma_free(wwr);
    // setup peers
    for (d=1; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        cudaDeviceDisablePeerAccess( gpu_id );
    }
}

void device_daxpy(int n, double *alpha, double *x, int incx, double *y, int incy, magma_queue_t *queue) {
    cublasHandle_t handle = magma_queue_get_cublas_handle( *queue );
    cublasSetPointerMode( handle, CUBLAS_POINTER_MODE_DEVICE );
    cublasDaxpy( handle, n, alpha, x, incx, y, incy );
    cublasSetPointerMode( handle, CUBLAS_POINTER_MODE_HOST );
}

//////////////////////////////////////////////////////////////////////
// BICG on multiple GPUs (distribute vector operation on all local GPUs)
void c_hacapk_bicgstab_cax_lfmtx_mgpu2_(stc_HACApK_leafmtxp *st_leafmtxp, stc_HACApK_lcontrol *st_ctl,
                                        double *u_cpu, double *b_cpu, double*param, int *nd, int *nstp, int *lrtrn) {
    // local constants
    int ione = 1;
    double zero =  0.0;
    double one  =  1.0;
    double mone = -1.0;
    // local arrays
    int *lpmd = (int*)((void*)st_ctl->param + st_ctl->lpmd_offset);
    int isct[2], irct[2];
    // local variables
    double eps, beta, zeta, zz, znormold, bnorm, zrnorm;
    #if defined(DDOT_REDUCE_ON_CPU)
    double alpha, zden, znorm;
    #endif
    double en_measure_time, st_measure_time, time;
    int info, step, mstep;
    int mpinr, nrank, ierr;
    double time_spmv, time_mpi, time_batch, time_set, time_copy, time_dot, tic, toc;
    double time_set1, time_set2, time_set3, time_set4; 

    MPI_Comm icomm = MPI_COMM_WORLD; //lpmd[0];
    mstep = param[82];
    eps = param[90];
    mpinr = lpmd[2]; 
    nrank = lpmd[1]; 
    MPI_Barrier( icomm );

    assert(gpus_per_proc == gpus_per_node);
    //int gpus_per_node = 4;
    //magma_device_t devices[ MagmaMaxGPUs ];
    //magma_getdevices( devices, MagmaMaxGPUs, &gpus_per_node );

    #if defined(ACCUM_ON_CPU)
    int flag = 0;
    #else
    int flag = 1;
    #endif
    int on_gpu = 1, d, gpu_id = (gpus_per_proc*get_device_id(st_leafmtxp))%gpus_per_node;
    magma_device_t cdev;
    magma_queue_t *queue = (magma_queue_t *)malloc(2*gpus_per_proc * sizeof(magma_queue_t));
    magma_event_t *event = (magma_event_t *)malloc(  gpus_per_proc * sizeof(magma_event_t));
    printf( " process %d on GPU(%d:%d)\n",mpinr,gpu_id,gpu_id+gpus_per_proc-1);
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_getdevice( &cdev );
        magma_queue_create( cdev, &queue[d] );
        magma_queue_create( cdev, &queue[d+gpus_per_proc] );
        magma_event_create( &event[d] );
    }
    // main GPU
    magma_setdevice(gpu_id);
    st_leafmtxp->iwork = (int*)malloc(2*gpus_per_proc * sizeof(int));

    // use pinned memory for CPU buffer
    double *zau_cpu1, *zau_cpu2, *wws_cpu, *wwr_cpu;
    magma_dmalloc_pinned(&zau_cpu1, *nd);
    magma_dmalloc_pinned(&zau_cpu2, *nd);
    magma_dmalloc_pinned(&wws_cpu,  *nd);
    magma_dmalloc_pinned(&wwr_cpu, (*nd)*gpus_per_proc);

    // allocate GPU vectors
    double *wws, *wwr;
    if (MAGMA_SUCCESS != magma_dmalloc(&wws, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc(&wwr, *nd)) {
      printf( " failed to allocate vectors (nd=%d)\n",*nd );
    }

    double **b = (double**)malloc(gpus_per_proc * sizeof(double*));
#define redundant_u
#if !defined(redundant_u)
    double **u = (double**)malloc(gpus_per_proc * sizeof(double*));
#endif
    double **zr = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zp = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zt = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zkp = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zkt = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zakp =(double**)malloc(gpus_per_proc * sizeof(double*));
    double **zshdw = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zakt = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **dBuffer = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **znorm_d = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **alpha_d = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zeta_d  = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **beta_d  = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **znorm_h = (double**)malloc(gpus_per_proc * sizeof(double*));
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        if (MAGMA_SUCCESS != magma_dmalloc(&b[d], *nd) ||
#if !defined(redundant_u)
            MAGMA_SUCCESS != magma_dmalloc(&u[d], *nd) ||
#endif
            MAGMA_SUCCESS != magma_dmalloc(&zt[d], 2*(*nd)) ||
            MAGMA_SUCCESS != magma_dmalloc(&zp[d], *nd) ||
            MAGMA_SUCCESS != magma_dmalloc(&zkp[d], *nd) ||
            MAGMA_SUCCESS != magma_dmalloc(&zkt[d], *nd) ||
            MAGMA_SUCCESS != magma_dmalloc(&zshdw[d], 3*(*nd)) ||
            MAGMA_SUCCESS != magma_dmalloc(&znorm_d[d], 4) ||
            MAGMA_SUCCESS != magma_dmalloc(&alpha_d[d], 2) ||
            MAGMA_SUCCESS != magma_dmalloc(&beta_d[d], 1) ||
            MAGMA_SUCCESS != magma_dmalloc(&zeta_d[d], 1)) {
          printf( " failed to allocate vectors (nd=%d)\n",*nd );
        }
        zr[d]   = &zshdw[d][*nd];
        zakp[d] = &zr[d][*nd];
        zakt[d] = &zt[d][*nd];
        magma_dmalloc_pinned(&znorm_h[d], 4);
    }
    magma_setdevice(gpu_id);
    for (d=0; d<gpus_per_proc; d++) {
        magma_dmalloc(&dBuffer[d], *nd);
    }
    // setup peers
    #if defined(DDOT_REDUCE_ON_CPU)
    for (d=1; d<gpus_per_proc; d++) {
        int canAccessPeer;
        magma_setdevice(gpu_id+d);
        cudaDeviceCanAccessPeer(&canAccessPeer, gpu_id+d, gpu_id);
        if (canAccessPeer == 1) {
            if (cudaDeviceEnablePeerAccess( gpu_id, 0 ) != cudaSuccess) {
                printf( " %d:%d: cudaDeviceEnablePeerAccess( %d ) failed\n",mpinr,gpu_id+d,gpu_id );
            }
        } else {
            printf( "cudaDeviceCanAccessPeer( %d, %d ) failed\n",gpu_id+d,gpu_id );
        }
    }
    #else
    for (d=0; d<gpus_per_proc; d++) {
        int dest;
        for (dest=0; dest<gpus_per_proc; dest++) {
            if (dest != d) {
                int canAccessPeer;
                magma_setdevice(gpu_id+d);
                cudaDeviceCanAccessPeer(&canAccessPeer, gpu_id+d, gpu_id+dest);
                if (canAccessPeer == 1) {
                    if (cudaDeviceEnablePeerAccess( gpu_id+dest, 0 ) != cudaSuccess) {
                        printf( " %d:%d: cudaDeviceEnablePeerAccess( %d ) failed\n",mpinr,gpu_id+d,gpu_id+dest );
                    }
                } else {
                    printf( "cudaDeviceCanAccessPeer( %d, %d ) failed\n",gpu_id+d,gpu_id+dest );
                }
            }
        }
    }
    #endif
    // MPI buffer
    double *buffer = NULL;
    int *disps = NULL;
    c_hacapk_adot_cax_lfmtx_comm_setup(st_ctl, &buffer, &disps, NULL);

    #ifdef WARMUP_MPI
    c_hacapk_adot_cax_lfmtx_warmup(st_ctl, zau_cpu, wws_cpu, wwr_cpu, *nd);
    #endif
    // copy matrix to GPU
    c_hacapk_adot_body_lfcpy_batch_sorted_mgpu_(nd, st_leafmtxp, queue);
#if defined(redundant_u)
    double **u = st_leafmtxp->zu_mgpu;
#endif
    //
    time_dot  = 0.0;
    time_spmv = 0.0;
    time_mpi = 0.0;
    time_batch = 0.0;
    time_set = 0.0;
    time_set1 = 0.0;
    time_set2 = 0.0;
    time_set3 = 0.0;
    time_set4 = 0.0;
    time_copy = 0.0;
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_queue_sync( queue[d] );
    }
    MPI_Barrier( icomm );
    st_measure_time = MPI_Wtime();
    // copy the input vector to GPU
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_dsetvector_async( *nd, b_cpu, 1, b[d], 1, queue[d] );
        magma_dsetvector_async( *nd, u_cpu, 1, u[d], 1, queue[d] );
    }
    // init
    #ifdef DDOT_REDUCE_ON_CPU
    alpha = 0.0; beta = 0.0; zeta = 0.0;
    #endif
    magma_setdevice(gpu_id);
    zz = magma_ddot(*nd, b[0], ione, b[0], ione, queue[0]); 
    bnorm=sqrt(zz);
    // .. SpMV ..
    int flag_set = 1;
    #if defined(PROF_MAGMA_BATCH)
    magma_queue_sync( queue[0] );
    tic = MPI_Wtime();
    #endif
    c_hacapk_adot_body_lfmtx_batch_mgpu2(0, zshdw[0],st_leafmtxp,st_ctl,u,wws, zau_cpu1,wwr_cpu,
                                         dBuffer, event, &time_batch,&time_set,&time_copy,
                                         &time_set1, &time_set2, &time_set3,
                                         on_gpu, queue, NULL, NULL);
    #if defined(PROF_MAGMA_BATCH)
    magma_queue_sync( queue[0] );
    time_spmv += (MPI_Wtime()-tic);
    #endif
    c_hacapk_adot_cax_lfmtx_comm_gpu(flag, zshdw[0], zau_cpu1,
                                     st_ctl,buffer,disps, wws_cpu, wwr_cpu, isct, irct, *nd, 
                                     &time_copy,&time_mpi, queue[0]);
    #if defined(PROF_MAGMA_BATCH)
    tic = MPI_Wtime();
    #endif
    #if 0
    if (nrank == 1) {
        magma_setdevice(gpu_id);
        magma_dgetvector( *nd, zshdw[0], 1, zau_cpu1, 1, queue[0] );
    }
    for (d=1; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_dsetvector_async( *nd, zau_cpu1, 1, zshdw[d], 1, queue[d] );
    }
    #else
    magma_event_record( event[0], queue[0] );
    for (d=1; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_queue_wait_event( queue[d], event[0] );
        cudaMemcpyPeerAsync(zshdw[d], gpu_id+d, zshdw[0], gpu_id, (*nd)*sizeof(double),
                            magma_queue_get_cuda_stream( queue[d] ) );
        magma_event_record( event[d], queue[d] );
        magma_queue_wait_event( queue[0], event[d] );
    }
    #endif
    #if defined(PROF_MAGMA_BATCH)
    for (d=1; d<gpus_per_proc; d++) {
        magma_queue_sync( queue[d] );
    }
    toc = (MPI_Wtime()-tic);
    time_set  += toc;
    time_set4 += toc;
    #endif
    //
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magmablas_dlacpy( MagmaFull, *nd, ione, b[d], *nd, zr[d], *nd, queue[d] );
        magma_daxpy( *nd, mone, zshdw[d], ione, zr[d], ione, queue[d] );
        magmablas_dlacpy( MagmaFull, *nd, ione, zr[d], *nd, zshdw[d], *nd, queue[d] );
    }
    magma_setdevice(gpu_id);
    zrnorm = magma_ddot(*nd, zr[0], ione, zr[0], ione, queue[0]); 
    zrnorm = sqrt(zrnorm);
    if (mpinr == 0) {
        printf( "\n ** BICG with MAGMA batched on multiple GPUs (version 2, %d GPUs) **\n",gpus_per_proc );
        printf( "\nOriginal relative residual norm = %.2e/%.2e = %.2e\n",zrnorm,bnorm,zrnorm/bnorm );
        printf( " first time_mpi=%.2e\n",time_mpi );
        printf( "HACApK_bicgstab_lfmtx_mgpu3 start\n" );
    }
    int nd_loc  = (*nd + 3)/4;
    for ( step=1; step<=mstep; step++ ) {
        if (zrnorm/bnorm < eps) break;
        // zp(:nd) = zr(:nd) + beta*(zp(:nd) - zeta*zakp(:nd))
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            if (step == 1) {
                magmablas_dlacpy( MagmaFull, *nd, ione, zr[d], *nd, zkp[d], *nd, queue[d] );
            } else {
                #ifdef DDOT_REDUCE_ON_CPU
                magma_daxpy( *nd, -zeta, zakp[d], ione, zp[d], ione, queue[d] );
                magmablas_dlacpy( MagmaFull, *nd, ione, zr[d], *nd, zkp[d], *nd, queue[d] );
                magma_daxpy( *nd, beta, zp[d], ione, zkp[d], ione, queue[d] );
                #else
                device_daxpy( *nd, zeta_d[d], zakp[d], ione, zp[d], ione, &queue[d] );
                magmablas_dlacpy( MagmaFull, *nd, ione, zr[d], *nd, zkp[d], *nd, queue[d] );
                device_daxpy( *nd, beta_d[d], zp[d], ione, zkp[d], ione, &queue[d] );
                #endif
            }
        }
        // zkp(:nd) = zp(:nd)
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magmablas_dlacpy( MagmaFull, *nd, ione, zkp[d], *nd, zp[d], *nd, queue[d] );
        }
        //  .. SpMV ..
        magma_setdevice(gpu_id);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue[0] );
        tic = MPI_Wtime();
        #endif
        c_hacapk_adot_body_lfmtx_batch_mgpu2(flag_set, zakp[0],st_leafmtxp,st_ctl, zkp,wws, zau_cpu2,wwr_cpu,
                                             dBuffer,event, &time_batch,&time_set,&time_copy, 
                                             &time_set1, &time_set2, &time_set3,
                                             on_gpu, queue, NULL, NULL);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue[0] );
        time_spmv += (MPI_Wtime()-tic);
        #endif
        c_hacapk_adot_cax_lfmtx_comm_gpu(flag, zakp[0], zau_cpu2,
                                         st_ctl,buffer,disps, wws_cpu,wwr_cpu, isct,irct,*nd, 
                                         &time_copy,&time_mpi, queue[0]);
        #if defined(PROF_MAGMA_BATCH)
        tic = MPI_Wtime();
        #endif
        #if 0
        if (nrank == 1) {
            magma_setdevice(gpu_id);
            magma_dgetvector( *nd, zakp[0], 1, zau_cpu2, 1, queue[0] );
        }
        for (d=1; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_dsetvector_async( *nd, zau_cpu2, 1, zakp[d], 1, queue[d] );
        }
        #else
        magma_event_record( event[0], queue[0] );
        for (d=1; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_queue_wait_event( queue[d], event[0] );
            cudaMemcpyPeerAsync(zakp[d], gpu_id+d, zakp[0], gpu_id, (*nd)*sizeof(double),
                                magma_queue_get_cuda_stream( queue[d] ) );
            magma_event_record( event[d], queue[d] );
            magma_queue_wait_event( queue[0], event[d] );
        }
        #endif
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        toc = (MPI_Wtime()-tic);
        time_set  += toc;
        time_set4 += toc;

        tic = MPI_Wtime();
        #endif
        // distributed
        int offset = 0, dd;
#if 0
        magma_setdevice(gpu_id);
        znorm = magma_ddot( *nd, zshdw[0], ione, zr[0], ione, queue[0] ); 
        zden  = magma_ddot( *nd, zshdw[0], ione, zakp[0], ione, queue[0] );
        alpha = -znorm/zden;
        znormold = znorm;
#else
        for (d=0; d<gpus_per_proc; d++) {
            int nd_d = (d == gpus_per_proc-1 ? (*nd)-d*nd_loc : nd_loc);
            magma_setdevice(d);
            #if defined(DDOT_BY_DGEMV)
            /*magma_dgemm( MagmaTrans, MagmaNoTrans,
                         2, 1, nd_d,
                         one, &(zr[d][offset]), *nd,
                              &(zshdw[d][offset]), *nd,
                         zero,  znorm_d[d], 2, queue[d] );*/
            /*magmablas_dgemv( MagmaTrans, nd_d, 2,
                             one,  &(zr[d][offset]), *nd,
                                   &(zshdw[d][offset]), ione,
                             zero, znorm_d[d], ione,
                             queue[d] );*/
            magma_dgemv( MagmaTrans, nd_d, 2,
                         one,  &(zr[d][offset]), *nd,
                               &(zshdw[d][offset]), ione,
                         zero, znorm_d[d], ione,
                         queue[d] );
            #else
            cublasSetPointerMode( magma_queue_get_cublas_handle( queue[d] ), CUBLAS_POINTER_MODE_DEVICE );
            cublasDdot( magma_queue_get_cublas_handle( queue[d] ), nd_d, 
                        &(zshdw[d][offset]), ione, &(zr[d][offset]), ione, &znorm_d[d][0] );
            cublasDdot( magma_queue_get_cublas_handle( queue[d] ), nd_d,
                        &(zshdw[d][offset]), ione, &(zakp[d][offset]), ione, &znorm_d[d][1] );
            cublasSetPointerMode(magma_queue_get_cublas_handle( queue[d] ), CUBLAS_POINTER_MODE_HOST);
            #endif
            if (d == 0) { // GPU-0 -> GPU->2
                cudaMemcpyPeerAsync( &znorm_d[2][2], gpu_id+2, znorm_d[0], gpu_id, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[0] ) );
                magma_event_record( event[0], queue[0] );
            } else if (d == 1) { // GPU-1 -> GPU-3
                cudaMemcpyPeerAsync( &znorm_d[3][2], gpu_id+3, znorm_d[1], gpu_id+1, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[1] ) );
                magma_event_record( event[1], queue[1] );
            } else if (d == 2) {
                magma_queue_wait_event( queue[2], event[0] );
                magma_daxpy( 2, one,  &znorm_d[2][2], 1, &znorm_d[2][0], 1, queue[2] );

                // copy it back, alpha[0]=-znorm/zden
                magma_dgetvector_async( 2, znorm_d[2], 1, znorm_h[2], 1, queue[2] );
            } else if (d == 3) {
                magma_queue_wait_event( queue[3], event[1] );
                magma_daxpy( 2, one,  &znorm_d[3][2], 1, &znorm_d[3][0], 1, queue[3] );

                // copy it back, alpha=-znorm/zden
                magma_dgetvector_async( 2, znorm_d[3], 1, znorm_h[3], 1, queue[3] );
            }
            offset += nd_d;
        }
        magma_queue_sync( queue[2] );
        magma_queue_sync( queue[3] );
        znorm = znorm_h[2][0]+znorm_h[3][0];
        zden  = znorm_h[2][1]+znorm_h[3][1];
        alpha = -znorm/zden;
        znormold = znorm;
#endif
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        time_dot += (MPI_Wtime()-tic);
        #endif
        //
        // zt(:nd) = zr(:nd) - alpha*zakp(:nd)
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magmablas_dlacpy( MagmaFull, *nd, ione, zr[d], *nd, zt[d], *nd, queue[d] );
            magma_daxpy( *nd, alpha, zakp[d], ione, zt[d], ione, queue[d] );
        }
        // zkt(:nd) = zt(:nd)
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magmablas_dlacpy( MagmaFull, *nd, ione, zt[d], *nd, zkt[d], *nd, queue[d] );
        }
        //  .. SpMV ..
        magma_setdevice(gpu_id);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue[0] );
        tic = MPI_Wtime();
        #endif
        c_hacapk_adot_body_lfmtx_batch_mgpu2(flag_set, zakt[0],st_leafmtxp,st_ctl, zkt,wws, zau_cpu1,wwr_cpu, 
                                             dBuffer,event, &time_batch,&time_set,&time_copy,
                                             &time_set1, &time_set2, &time_set3,
                                             on_gpu, queue, NULL, NULL);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue[0] );
        time_spmv += (MPI_Wtime()-tic);
        #endif
        c_hacapk_adot_cax_lfmtx_comm_gpu(flag, zakt[0],zau_cpu1,
                                         st_ctl,buffer,disps, wws_cpu,wwr_cpu, isct,irct,*nd, 
                                         &time_copy,&time_mpi, queue[0]);
        #if defined(PROF_MAGMA_BATCH)
        tic = MPI_Wtime();
        #endif
        #if 0
        if (nrank == 1) {
            magma_setdevice(gpu_id);
            magma_dgetvector( *nd, zakt[0], 1, zau_cpu1, 1, queue[0] );
        }
        for (d=1; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_dsetvector_async( *nd, zau_cpu1, 1, zakt[d], 1, queue[d] );
        }
        #else
        magma_event_record( event[0], queue[0] );
        for (d=1; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_queue_wait_event( queue[d], event[0] );
            cudaMemcpyPeerAsync(zakt[d], gpu_id+d, zakt[0], gpu_id, (*nd)*sizeof(double),
                                magma_queue_get_cuda_stream( queue[d] ) );
            magma_event_record( event[d], queue[d] );
            magma_queue_wait_event( queue[0], event[d] );
        }
        #endif
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        toc = (MPI_Wtime()-tic);
        time_set  += toc;
        time_set4 += toc;

        tic = MPI_Wtime();
        #endif
        //
#if 0
        magma_setdevice(gpu_id);
        znorm = magma_ddot( *nd, zakt[0], ione, zt[0], ione, queue[0] ); 
        zden  = magma_ddot( *nd, zakt[0], ione, zakt[0], ione, queue[0] );
        zeta = znorm/zden;
#else
        offset = 0;
        for (d=0; d<gpus_per_proc; d++) {
            int nd_d = (d == gpus_per_proc-1 ? (*nd)-d*nd_loc : nd_loc);
            magma_setdevice(d);
            #if defined(DDOT_BY_DGEMV)
            /*magma_dgemm( MagmaTrans, MagmaNoTrans,
                         2, 1, nd_d,
                         one, &(zt[d][offset]), *nd,
                              &(zakt[d][offset]), *nd,
                         zero,  znorm_d[d], 2, queue[d] );*/
            /*magmablas_dgemv( MagmaTrans, nd_d, 2,
                             one,  &(zt[d][offset]), *nd,
                                   &(zakt[d][offset]), ione,
                             zero, znorm_d[d], ione,
                             queue[d] );*/
            magma_dgemv( MagmaTrans, nd_d, 2,
                         one,  &(zt[d][offset]), *nd,
                               &(zakt[d][offset]), ione,
                         zero, znorm_d[d], ione,
                         queue[d] );
            #else
            cublasSetPointerMode( magma_queue_get_cublas_handle( queue[d] ), CUBLAS_POINTER_MODE_DEVICE );
            cublasDdot( magma_queue_get_cublas_handle( queue[d] ), nd_d, 
                        &(zakt[d][offset]), ione, &(zt[d][offset]), ione, &znorm_d[d][0] );
            cublasDdot( magma_queue_get_cublas_handle( queue[d] ), nd_d,
                        &(zakt[d][offset]), ione, &(zakt[d][offset]), ione, &znorm_d[d][1] );
            cublasSetPointerMode(magma_queue_get_cublas_handle( queue[d] ), CUBLAS_POINTER_MODE_HOST);
            #endif
            if (d == 0) { // GPU-0 -> GPU->2
                cudaMemcpyPeerAsync( &znorm_d[2][2], gpu_id+2, znorm_d[0], gpu_id, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[0] ) );
                magma_event_record( event[0], queue[0] );
            } else if (d == 1) { // GPU-1 -> GPU-3
                cudaMemcpyPeerAsync( &znorm_d[3][2], gpu_id+3, znorm_d[1], gpu_id+1, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[1] ) );
                magma_event_record( event[1], queue[1] );
            } else if (d == 2) {
                magma_queue_wait_event( queue[2], event[0] );
                magma_daxpy( 2, one,  &znorm_d[2][2], 1, &znorm_d[2][0], 1, queue[2] );

                // copy it back, zeta = znorm/zden
                magma_dgetvector_async( 2, znorm_d[2], 1, znorm_h[2], 1, queue[2] );
            } else if (d == 3) {
                magma_queue_wait_event( queue[3], event[1] );
                magma_daxpy( 2, one,  &znorm_d[d][2], 1, &znorm_d[d][0], 1, queue[d] );

                // copy it back, zeta = znorm/zden
                magma_dgetvector_async( 2, znorm_d[3], 1, znorm_h[3], 1, queue[3] );
            }
            offset += nd_d;
        }
        magma_queue_sync( queue[2] );
        magma_queue_sync( queue[3] );
        znorm = znorm_h[2][0]+znorm_h[3][0];
        zden  = znorm_h[2][1]+znorm_h[3][1];
        zeta = znorm/zden;
#endif
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        time_dot += (MPI_Wtime()-tic);
        #endif
        //
        // u(:nd) = u(:nd) + alpha*zkp(:nd) + zeta*zkt(:nd) (distributed)
#if 0
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_daxpy( *nd, alpha, zkp[d], ione, u[d], ione, queue[d] );
            magma_daxpy( *nd, zeta,  zkt[d], ione, u[d], ione, queue[d] );
        }
#else
        offset = 0;
        for (d=0; d<gpus_per_proc; d++) {
            int nd_d = (d == gpus_per_proc-1 ? (*nd)-d*nd_loc : nd_loc);
            magma_setdevice(gpu_id+d);
            magma_daxpy( nd_d, alpha, &(zkp[d][offset]), ione, &(u[d][offset]), ione, queue[d] );
            magma_daxpy( nd_d, zeta,  &(zkt[d][offset]), ione, &(u[d][offset]), ione, queue[d] );
            offset += nd_d;
        }
#endif
        // zr(:nd) = zt(:nd) - zeta*zakt(:nd)
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magmablas_dlacpy( MagmaFull, *nd, ione, zt[d], *nd, zr[d], *nd, queue[d] );
            #if defined(DDOT_REDUCE_ON_CPU)
            magma_daxpy( *nd, -zeta, zakt[d], ione, zr[d], ione, queue[d] );
            #else
            magmablas_dlascl( MagmaFull, ione, ione, -one, one, ione, ione, zeta_d[d], ione, queue[d], &info );
            device_daxpy( *nd, zeta_d[d], zakt[d], ione, zr[d], ione, &queue[d] );
            #endif
        }
        // beta = alpha/zeta * HACApK_dotp_d(nd,zshdw,zr)/znormold;
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        tic = MPI_Wtime();
        #endif
#if 0
        magma_setdevice(gpu_id);
        beta   = magma_ddot( *nd, zshdw[0], ione, zr[0], ione, queue[0]);
        zrnorm = magma_ddot( *nd, zr[0],    ione, zr[0], ione, queue[0] );
        beta = -alpha/zeta * beta/znormold;
        zrnorm = sqrt(zrnorm);
#else
        offset = 0;
        for (d=0; d<gpus_per_proc; d++) {
            int nd_d = (d == gpus_per_proc-1 ? (*nd)-d*nd_loc : nd_loc);
            magma_setdevice(d);
            #if defined(DDOT_BY_DGEMV)
            /*magma_dgemm( MagmaTrans, MagmaNoTrans, 2, 1, nd_d,
                         one, &(zshdw[d][offset]), *nd,
                              &(zr[d][offset]), *nd,
                         zero,  znorm_d[d], 2, queue[d] );*/
            /*magmablas_dgemv( MagmaTrans, nd_d, 2,
                             one,  &(zshdw[d][offset]), *nd,
                                   &(zr[d][offset]), ione,
                             zero, znorm_d[d], ione,
                             queue[d] );*/
            magma_dgemv( MagmaTrans, nd_d, 2,
                         one,  &(zshdw[d][offset]), *nd,
                               &(zr[d][offset]), ione,
                         zero, znorm_d[d], ione,
                         queue[d] );
            #else
            cublasSetPointerMode( magma_queue_get_cublas_handle( queue[d] ), CUBLAS_POINTER_MODE_DEVICE );
            cublasDdot( magma_queue_get_cublas_handle( queue[d] ), nd_d, 
                        &(zr[d][offset]), ione, &(zshdw[d][offset]), ione, &znorm_d[d][0] );
            cublasDdot( magma_queue_get_cublas_handle( queue[d] ), nd_d,
                        &(zr[d][offset]), ione, &(zr[d][offset]), ione, &znorm_d[d][1] );
            cublasSetPointerMode(magma_queue_get_cublas_handle( queue[d] ), CUBLAS_POINTER_MODE_HOST);
            #endif
            if (d == 0) { // GPU-0 -> GPU->2
                cudaMemcpyPeerAsync( &znorm_d[2][2], gpu_id+2, znorm_d[0], gpu_id, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[0] ) );
                magma_event_record( event[0], queue[0] );
            } else if (d == 1) { // GPU-1 -> GPU-3
                cudaMemcpyPeerAsync( &znorm_d[3][2], gpu_id+3, znorm_d[1], gpu_id+1, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[1] ) );
                magma_event_record( event[1], queue[1] );
            } else if (d == 2) {
                magma_queue_wait_event( queue[2], event[0] );
                magma_daxpy( 2, one,  &znorm_d[2][2], 1, &znorm_d[2][0], 1, queue[2] );
                // copy it back, alpha=-znorm/zden
                magma_dgetvector_async( 2, znorm_d[2], 1, znorm_h[2], 1, queue[2] );
            } else if (d == 3) {
                magma_queue_wait_event( queue[3], event[1] );
                magma_daxpy( 2, one,  &znorm_d[3][2], 1, &znorm_d[3][0], 1, queue[3] );
                // for convergence check.
                magma_dgetvector_async( 2, znorm_d[3], 1, znorm_h[3], 1, queue[3] );
            }
            offset += nd_d;
        }
        //
        //beta *= (alpha/zeta) / znormold;
        magma_queue_sync( queue[2] );
        magma_queue_sync( queue[3] );
        beta = znorm_h[2][0]+znorm_h[3][0];
        beta = -alpha/zeta * beta/znormold;
        zrnorm = sqrt(znorm_h[2][1]+znorm_h[3][1]);
#endif
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        time_dot += (MPI_Wtime()-tic);
        #endif

        *nstp = step;
        en_measure_time = MPI_Wtime();
        time = en_measure_time - st_measure_time;
        if (st_ctl->param[0] > 0 && mpinr == 0) {
            printf( " %d: time=%.2e log10(zrnorm/bnorm)=log10(%.2e/%.2e)=%.2e\n",step,time,zrnorm,bnorm,log10(zrnorm/bnorm) );
        }
    }
    //magma_dgetvector( *nd, u[0], 1, u_cpu, 1, queue[0] );
    int offset = 0;
    for (d=0; d<gpus_per_proc; d++) {
        int nd_d = (d == gpus_per_proc-1 ? *nd-d*nd_loc : nd_loc);
        magma_setdevice(gpu_id+d);
        magma_dgetvector( nd_d, &(u[d][offset]), 1, &(u_cpu[offset]), 1, queue[d] );
    }
    MPI_Barrier( icomm );
    en_measure_time = MPI_Wtime();
    time = en_measure_time - st_measure_time;
    if (st_ctl->param[0]>0) {
        //printf( " End: %d, %.2e\n",mpinr,time );
        if (mpinr == 0) {
            printf( "       BiCG       = %.5e\n", time );
            printf( "        time_mpi  = %.5e\n", time_mpi );
            printf( "        time_copy = %.5e\n", time_copy );
            printf( "        time_spmv = %.5e\n", time_spmv );
            printf( "        > time_batch = %.5e\n", time_batch );
            printf( "        > time_set   = %.5e\n", time_set );
            printf( "          + time_set1 = %.5e\n", time_set1 );
            printf( "          + time_set2 = %.5e\n", time_set2 );
            printf( "          + time_set3 = %.5e\n", time_set3 );
            printf( "          + time_set4 = %.5e\n", time_set4 );
            printf( "          +          += %.5e\n", time_set1+time_set2+time_set3+time_set4 );
            printf( "        > time_dot   = %.5e\n", time_dot );
            printf( "                     => %.5e\n", time_mpi+time_copy+time_spmv+time_dot );
        }
    }

    // free gpu memory
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_queue_sync( queue[d] );
        magma_queue_destroy( queue[d] );
        magma_queue_destroy( queue[d+gpus_per_proc] );

#if !defined(redundant_u)
        magma_free(u[d]);
#endif
        magma_free(b[d]);

        magma_free(zt[d]);
        magma_free(zp[d]);
        magma_free(zkp[d]);
        magma_free(zakp[d]);
        magma_free(zkt[d]);
        magma_free(zakt[d]);
        magma_free(zshdw[d]);
    }
    // delete matrix
    c_hacapk_adot_body_lfdel_mgpu_(st_leafmtxp);

    magma_setdevice(gpu_id);
    // free cpu memory
    free(queue);
    if (buffer != NULL) free(buffer);
    free(disps);
#if !defined(redundant_u)
    free(u);
#endif
    free(b);
    free(zt);
    free(zr);
    free(zp);
    free(zkp);
    free(zkt);
    free(zshdw);

    magma_free_pinned(zau_cpu1);
    magma_free_pinned(zau_cpu2);
    magma_free_pinned(wws_cpu);
    magma_free_pinned(wwr_cpu);

    magma_free(wws);
    magma_free(wwr);
    // setup peers
    for (d=1; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        int dest;
        for (dest=0; dest<gpus_per_proc; dest++) {
            if (dest != d) cudaDeviceDisablePeerAccess( gpu_id+dest );
        }
    }
}

//////////////////////////////////////////////////////////////////////
// BICG on multiple GPUs (distribute vector operation on local GPUs, specialized for Tsubame..)
void c_hacapk_bicgstab_cax_lfmtx_mgpu3_(stc_HACApK_leafmtxp *st_leafmtxp, stc_HACApK_lcontrol *st_ctl,
                                        double *u_cpu, double *b_cpu, double*param, int *nd, int *nstp, int *lrtrn) {
    // local constants
    int ione = 1;
    double zero =  0.0;
    double one  =  1.0;
    double mone = -1.0;
    // local arrays
    int *lpmd = (int*)((void*)st_ctl->param + st_ctl->lpmd_offset);
    int isct[2], irct[2];
    // local variables
    double eps, beta, zeta, zz, znormold, bnorm, zrnorm;
    #if defined(DDOT_REDUCE_ON_CPU)
    double alpha, zden, znorm;
    #endif
    double en_measure_time, st_measure_time, time;
    int info, step, mstep;
    int mpinr, nrank, ierr;
    double time_spmv, time_mpi, time_batch, time_set, time_copy, time_dot, tic, toc;
    double time_set1, time_set2, time_set3, time_set4; 

    MPI_Comm icomm = MPI_COMM_WORLD; //lpmd[0];
    mstep = param[82];
    eps = param[90];
    mpinr = lpmd[2]; 
    nrank = lpmd[1]; 
    MPI_Barrier( icomm );

    assert(gpus_per_proc == gpus_per_node);
    //int gpus_per_node = 4;
    //magma_device_t devices[ MagmaMaxGPUs ];
    //magma_getdevices( devices, MagmaMaxGPUs, &gpus_per_node );

    #if defined(ACCUM_ON_CPU)
    int flag = 0;
    #else
    int flag = 1;
    #endif
    int on_gpu = 1, d, gpu_id = (gpus_per_proc*get_device_id(st_leafmtxp))%gpus_per_node;
    magma_device_t cdev;
    magma_queue_t *queue = (magma_queue_t *)malloc(2*gpus_per_proc * sizeof(magma_queue_t));
    magma_event_t *event = (magma_event_t *)malloc(  gpus_per_proc * sizeof(magma_event_t));
    printf( " process %d on GPU(%d:%d)\n",mpinr,gpu_id,gpu_id+gpus_per_proc-1);
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_getdevice( &cdev );
        magma_queue_create( cdev, &queue[d] );
        magma_queue_create( cdev, &queue[d+gpus_per_proc] );
        magma_event_create( &event[d] );
    }
    // main GPU
    magma_setdevice(gpu_id);
    st_leafmtxp->iwork = (int*)malloc(2*gpus_per_proc * sizeof(int));

    // use pinned memory for CPU buffer
    double *zau_cpu1, *zau_cpu2, *wws_cpu, *wwr_cpu;
    magma_dmalloc_pinned(&zau_cpu1, *nd);
    magma_dmalloc_pinned(&zau_cpu2, *nd);
    magma_dmalloc_pinned(&wws_cpu,  *nd);
    magma_dmalloc_pinned(&wwr_cpu, (*nd)*gpus_per_proc);

    // allocate GPU vectors
    double *wws, *wwr;
    if (MAGMA_SUCCESS != magma_dmalloc(&wws, *nd) ||
        MAGMA_SUCCESS != magma_dmalloc(&wwr, *nd)) {
      printf( " failed to allocate vectors (nd=%d)\n",*nd );
    }

    double **b = (double**)malloc(gpus_per_proc * sizeof(double*));
#define redundant_u
#if !defined(redundant_u)
    double **u = (double**)malloc(gpus_per_proc * sizeof(double*));
#endif
    double **zr = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zp = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zt = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zkp = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zkt = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zakp =(double**)malloc(gpus_per_proc * sizeof(double*));
    double **zshdw = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zakt = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **dBuffer = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **znorm_d = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **alpha_d = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **zeta_d  = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **beta_d  = (double**)malloc(gpus_per_proc * sizeof(double*));
    double **znorm_h = (double**)malloc(gpus_per_proc * sizeof(double*));
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        if (MAGMA_SUCCESS != magma_dmalloc(&b[d], *nd) ||
#if !defined(redundant_u)
            MAGMA_SUCCESS != magma_dmalloc(&u[d], *nd) ||
#endif
            MAGMA_SUCCESS != magma_dmalloc(&zt[d], 2*(*nd)) ||
            MAGMA_SUCCESS != magma_dmalloc(&zp[d], *nd) ||
            MAGMA_SUCCESS != magma_dmalloc(&zkp[d], *nd) ||
            MAGMA_SUCCESS != magma_dmalloc(&zkt[d], *nd) ||
            MAGMA_SUCCESS != magma_dmalloc(&zshdw[d], 3*(*nd)) ||
            MAGMA_SUCCESS != magma_dmalloc(&znorm_d[d], 4) ||
            MAGMA_SUCCESS != magma_dmalloc(&alpha_d[d], 2) ||
            MAGMA_SUCCESS != magma_dmalloc(&beta_d[d], 1) ||
            MAGMA_SUCCESS != magma_dmalloc(&zeta_d[d], 1)) {
          printf( " failed to allocate vectors (nd=%d)\n",*nd );
        }
        zr[d]   = &zshdw[d][*nd];
        zakp[d] = &zr[d][*nd];
        zakt[d] = &zt[d][*nd];
        magma_dmalloc_pinned(&znorm_h[d], 4);
    }
    magma_setdevice(gpu_id);
    for (d=0; d<gpus_per_proc; d++) {
        magma_dmalloc(&dBuffer[d], *nd);
    }
    // setup peers
    #if defined(DDOT_REDUCE_ON_CPU)
    for (d=1; d<gpus_per_proc; d++) {
        int canAccessPeer;
        magma_setdevice(gpu_id+d);
        cudaDeviceCanAccessPeer(&canAccessPeer, gpu_id+d, gpu_id);
        if (canAccessPeer == 1) {
            if (cudaDeviceEnablePeerAccess( gpu_id, 0 ) != cudaSuccess) {
                printf( " %d:%d: cudaDeviceEnablePeerAccess( %d ) failed\n",mpinr,gpu_id+d,gpu_id );
            }
        } else {
            printf( "cudaDeviceCanAccessPeer( %d, %d ) failed\n",gpu_id+d,gpu_id );
        }
    }
    #else
    for (d=0; d<gpus_per_proc; d++) {
        int dest;
        for (dest=0; dest<gpus_per_proc; dest++) {
            if (dest != d) {
                int canAccessPeer;
                magma_setdevice(gpu_id+d);
                cudaDeviceCanAccessPeer(&canAccessPeer, gpu_id+d, gpu_id+dest);
                if (canAccessPeer == 1) {
                    if (cudaDeviceEnablePeerAccess( gpu_id+dest, 0 ) != cudaSuccess) {
                        printf( " %d:%d: cudaDeviceEnablePeerAccess( %d ) failed\n",mpinr,gpu_id+d,gpu_id+dest );
                    }
                } else {
                    printf( "cudaDeviceCanAccessPeer( %d, %d ) failed\n",gpu_id+d,gpu_id+dest );
                }
            }
        }
    }
    #endif
    // MPI buffer
    double *buffer = NULL;
    int *disps = NULL;
    c_hacapk_adot_cax_lfmtx_comm_setup(st_ctl, &buffer, &disps, NULL);

    #ifdef WARMUP_MPI
    c_hacapk_adot_cax_lfmtx_warmup(st_ctl, zau_cpu, wws_cpu, wwr_cpu, *nd);
    #endif
    // copy matrix to GPU
    c_hacapk_adot_body_lfcpy_batch_sorted_mgpu_(nd, st_leafmtxp, queue);
#if defined(redundant_u)
    double **u = st_leafmtxp->zu_mgpu;
#endif
    //
    time_dot  = 0.0;
    time_spmv = 0.0;
    time_mpi = 0.0;
    time_batch = 0.0;
    time_set = 0.0;
    time_set1 = 0.0;
    time_set2 = 0.0;
    time_set3 = 0.0;
    time_set4 = 0.0;
    time_copy = 0.0;
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_queue_sync( queue[d] );
    }
    MPI_Barrier( icomm );
    st_measure_time = MPI_Wtime();
    // copy the input vector to GPU
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_dsetvector_async( *nd, b_cpu, 1, b[d], 1, queue[d] );
        magma_dsetvector_async( *nd, u_cpu, 1, u[d], 1, queue[d] );
    }
    // init
    #ifdef DDOT_REDUCE_ON_CPU
    alpha = 0.0; beta = 0.0; zeta = 0.0;
    #endif
    magma_setdevice(gpu_id);
    zz = magma_ddot(*nd, b[0], ione, b[0], ione, queue[0]); 
    bnorm=sqrt(zz);
    // .. SpMV ..
    int flag_set = 1;
    #if defined(PROF_MAGMA_BATCH)
    magma_queue_sync( queue[0] );
    tic = MPI_Wtime();
    #endif
    c_hacapk_adot_body_lfmtx_batch_mgpu2(0, zshdw[0],st_leafmtxp,st_ctl,u,wws, zau_cpu1,wwr_cpu,
                                         dBuffer, event, &time_batch,&time_set,&time_copy,
                                         &time_set1, &time_set2, &time_set3,
                                         on_gpu, queue, NULL, NULL);
    #if defined(PROF_MAGMA_BATCH)
    magma_queue_sync( queue[0] );
    time_spmv += (MPI_Wtime()-tic);
    #endif
    c_hacapk_adot_cax_lfmtx_comm_gpu(flag, zshdw[0], zau_cpu1,
                                     st_ctl,buffer,disps, wws_cpu, wwr_cpu, isct, irct, *nd, 
                                     &time_copy,&time_mpi, queue[0]);
    #if defined(PROF_MAGMA_BATCH)
    tic = MPI_Wtime();
    #endif
    #if 0
    if (nrank == 1) {
        magma_setdevice(gpu_id);
        magma_dgetvector( *nd, zshdw[0], 1, zau_cpu1, 1, queue[0] );
    }
    for (d=1; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_dsetvector_async( *nd, zau_cpu1, 1, zshdw[d], 1, queue[d] );
    }
    #else
    magma_event_record( event[0], queue[0] );
    for (d=1; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_queue_wait_event( queue[d], event[0] );
        cudaMemcpyPeerAsync(zshdw[d], gpu_id+d, zshdw[0], gpu_id, (*nd)*sizeof(double),
                            magma_queue_get_cuda_stream( queue[d] ) );
        magma_event_record( event[d], queue[d] );
        magma_queue_wait_event( queue[0], event[d] );
    }
    #endif
    #if defined(PROF_MAGMA_BATCH)
    for (d=1; d<gpus_per_proc; d++) {
        magma_queue_sync( queue[d] );
    }
    toc = (MPI_Wtime()-tic);
    time_set  += toc;
    time_set4 += toc;
    #endif
    //
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magmablas_dlacpy( MagmaFull, *nd, ione, b[d], *nd, zr[d], *nd, queue[d] );
        magma_daxpy( *nd, mone, zshdw[d], ione, zr[d], ione, queue[d] );
        magmablas_dlacpy( MagmaFull, *nd, ione, zr[d], *nd, zshdw[d], *nd, queue[d] );
    }
    magma_setdevice(gpu_id);
    zrnorm = magma_ddot(*nd, zr[0], ione, zr[0], ione, queue[0]); 
    zrnorm = sqrt(zrnorm);
    if (mpinr == 0) {
        printf( "\n ** BICG with MAGMA batched on multiple GPUs (version 3, %d GPUs) **\n",gpus_per_proc );
        printf( "\nOriginal relative residual norm = %.2e/%.2e = %.2e\n",zrnorm,bnorm,zrnorm/bnorm );
        printf( " first time_mpi=%.2e\n",time_mpi );
        printf( "HACApK_bicgstab_lfmtx_mgpu3 start\n" );
    }
    int nd_loc2 = (*nd + 1)/2;
    int nd_loc  = (*nd + 3)/4;
    for ( step=1; step<=mstep; step++ ) {
        if (zrnorm/bnorm < eps) break;
        // zp(:nd) = zr(:nd) + beta*(zp(:nd) - zeta*zakp(:nd))
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            if (step == 1) {
                magmablas_dlacpy( MagmaFull, *nd, ione, zr[d], *nd, zkp[d], *nd, queue[d] );
            } else {
                #ifdef DDOT_REDUCE_ON_CPU
                magma_daxpy( *nd, -zeta, zakp[d], ione, zp[d], ione, queue[d] );
                magmablas_dlacpy( MagmaFull, *nd, ione, zr[d], *nd, zkp[d], *nd, queue[d] );
                magma_daxpy( *nd, beta, zp[d], ione, zkp[d], ione, queue[d] );
                #else
                device_daxpy( *nd, zeta_d[d], zakp[d], ione, zp[d], ione, &queue[d] );
                magmablas_dlacpy( MagmaFull, *nd, ione, zr[d], *nd, zkp[d], *nd, queue[d] );
                device_daxpy( *nd, beta_d[d], zp[d], ione, zkp[d], ione, &queue[d] );
                #endif
            }
        }
        // zkp(:nd) = zp(:nd)
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magmablas_dlacpy( MagmaFull, *nd, ione, zkp[d], *nd, zp[d], *nd, queue[d] );
        }
        //  .. SpMV ..
        magma_setdevice(gpu_id);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue[0] );
        tic = MPI_Wtime();
        #endif
        c_hacapk_adot_body_lfmtx_batch_mgpu2(flag_set, zakp[0],st_leafmtxp,st_ctl, zkp,wws, zau_cpu2,wwr_cpu,
                                             dBuffer,event, &time_batch,&time_set,&time_copy, 
                                             &time_set1, &time_set2, &time_set3,
                                             on_gpu, queue, NULL, NULL);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue[0] );
        time_spmv += (MPI_Wtime()-tic);
        #endif
        c_hacapk_adot_cax_lfmtx_comm_gpu(flag, zakp[0], zau_cpu2,
                                         st_ctl,buffer,disps, wws_cpu,wwr_cpu, isct,irct,*nd, 
                                         &time_copy,&time_mpi, queue[0]);
        #if defined(PROF_MAGMA_BATCH)
        tic = MPI_Wtime();
        #endif
        #if 0
        if (nrank == 1) {
            magma_setdevice(gpu_id);
            magma_dgetvector( *nd, zakp[0], 1, zau_cpu2, 1, queue[0] );
        }
        for (d=1; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_dsetvector_async( *nd, zau_cpu2, 1, zakp[d], 1, queue[d] );
        }
        #else
        magma_event_record( event[0], queue[0] );
        for (d=1; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_queue_wait_event( queue[d], event[0] );
            cudaMemcpyPeerAsync(zakp[d], gpu_id+d, zakp[0], gpu_id, (*nd)*sizeof(double),
                                magma_queue_get_cuda_stream( queue[d] ) );
            magma_event_record( event[d], queue[d] );
            magma_queue_wait_event( queue[0], event[d] );
        }
        #endif
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        toc = (MPI_Wtime()-tic);
        time_set  += toc;
        time_set4 += toc;

        tic = MPI_Wtime();
        #endif
        // distributed
        int dd0, inc_dd, swc_dd;
        #ifdef DDOT_REDUCE_ON_CPU
        dd0 = 2;
        inc_dd = 2;
        swc_dd = 0;
        #else
        dd0 = 1;
        inc_dd = 1;
        swc_dd = 1;
        #endif
        int offset = 0, dd;
#if 0
        magma_setdevice(gpu_id);
        znorm = magma_ddot( *nd, zshdw[0], ione, zr[0], ione, queue[0] ); 
        zden  = magma_ddot( *nd, zshdw[0], ione, zakp[0], ione, queue[0] );
        alpha = -znorm/zden;
        znormold = znorm;
#else
        for (dd=0; dd<gpus_per_proc; dd+=inc_dd) {
            d = (dd0+dd)%gpus_per_proc;
            int nd_d = (dd > swc_dd ? *nd-nd_loc2 : nd_loc2);
            magma_setdevice(gpu_id+d);
            #if defined(DDOT_BY_DGEMV)
            /*magma_dgemm( MagmaTrans, MagmaNoTrans,
                         2, 1, nd_d,
                         one, &(zr[d][offset]), *nd,
                              &(zshdw[d][offset]), *nd,
                         zero,  znorm_d[d], 2, queue[d] );*/
            /*magmablas_dgemv( MagmaTrans, nd_d, 2,
                             one,  &(zr[d][offset]), *nd,
                                   &(zshdw[d][offset]), ione,
                             zero, znorm_d[d], ione,
                             queue[d] );*/
            magma_dgemv( MagmaTrans, nd_d, 2,
                         one,  &(zr[d][offset]), *nd,
                               &(zshdw[d][offset]), ione,
                         zero, znorm_d[d], ione,
                         queue[d] );
            #else
            cublasSetPointerMode( magma_queue_get_cublas_handle( queue[d] ), CUBLAS_POINTER_MODE_DEVICE );
            cublasDdot( magma_queue_get_cublas_handle( queue[d] ), nd_d, 
                        &(zshdw[d][offset]), ione, &(zr[d][offset]), ione, &znorm_d[d][0] );
            cublasDdot( magma_queue_get_cublas_handle( queue[d] ), nd_d,
                        &(zshdw[d][offset]), ione, &(zakp[d][offset]), ione, &znorm_d[d][1] );
            cublasSetPointerMode(magma_queue_get_cublas_handle( queue[d] ), CUBLAS_POINTER_MODE_HOST);
            #endif
            if (d == 1) { // GPU-1 -> GPU-3
                cudaMemcpyPeerAsync( &znorm_d[3][2], gpu_id+3, znorm_d[1], gpu_id+1, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[1] ) );
                magma_event_record( event[1], queue[1] );
            } else if (d == 2) { // GPU-2 -> GPU->0
                cudaMemcpyPeerAsync( &znorm_d[0][2], gpu_id+0, znorm_d[d], gpu_id+2, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[2] ) );
                magma_event_record( event[2], queue[2] );
            } else if (d == 3) {
                magma_queue_wait_event( queue[3], event[1] );
                magma_daxpy( 2, one,  &znorm_d[3][2], 1, &znorm_d[3][0], 1, queue[3] );

                // compute, copy it back, alpha=-znorm/zden
                // alpha[1] = znormold
                #if defined(DDOT_REDUCE_ON_CPU)
                magma_dgetvector( 2, znorm_d[3], 1, znorm_h[3], 1, queue[3] );
                znorm = znorm_h[3][0];
                zden  = znorm_h[3][1];
                alpha = -znorm/zden;
                znormold = znorm;
                #else
                magma_setdevice(gpu_id+3);
                magmablas_dlacpy( MagmaFull, 2, 1, znorm_d[3], 2, alpha_d[3], 2, queue[3] );
                magma_dtrsm( MagmaRight, MagmaUpper, MagmaNoTrans, MagmaNonUnit,
                             ione, ione,
                            -one, &znorm_d[3][1], ione,
                                   alpha_d[3], ione, queue[3] );
                cudaMemcpyPeerAsync( alpha_d[1], gpu_id+1, alpha_d[3], gpu_id+3, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[3] ) );
                magma_event_record( event[3], queue[3] );
                magma_queue_wait_event( queue[1], event[3] );
                #endif
            } else if (d == 0) {
                magma_queue_wait_event( queue[0], event[2] );
                magma_daxpy( 2, one,  &znorm_d[0][2], 1, &znorm_d[0][0], 1, queue[0] );

                // compute, copy it back, alpha[0]=-znorm/zden
                // alpha[1] = znormold
                #if defined(DDOT_REDUCE_ON_CPU)
                magma_dgetvector( 2, znorm_d[0], 1, znorm_h[0], 1, queue[0] );
                znorm = znorm_h[0][0];
                zden  = znorm_h[0][1];
                alpha = -znorm/zden;
                znormold = znorm;
                #else
                magmablas_dlacpy( MagmaFull, 2, 1, znorm_d[0], 2, alpha_d[0], 2, queue[0] );
                magma_dtrsm( MagmaRight, MagmaUpper, MagmaNoTrans, MagmaNonUnit,
                             ione, ione,
                            -one, &znorm_d[0][1], ione,
                                   alpha_d[0], ione, queue[0] );
                cudaMemcpyPeerAsync( alpha_d[2], gpu_id+2, alpha_d[0], gpu_id+0, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[0] ) );
                magma_event_record( event[0], queue[0] );
                magma_queue_wait_event( queue[2], event[0] );
                #endif
            }
            if (dd == swc_dd) offset += nd_d;
        }
#endif
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        time_dot += (MPI_Wtime()-tic);
        #endif
        //
        // zt(:nd) = zr(:nd) - alpha*zakp(:nd)
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magmablas_dlacpy( MagmaFull, *nd, ione, zr[d], *nd, zt[d], *nd, queue[d] );
            #if defined(DDOT_REDUCE_ON_CPU)
            magma_daxpy( *nd, alpha, zakp[d], ione, zt[d], ione, queue[d] );
            #else
            device_daxpy( *nd, alpha_d[d], zakp[d], ione, zt[d], ione, &queue[d] );
            #endif
        }
        // zkt(:nd) = zt(:nd)
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magmablas_dlacpy( MagmaFull, *nd, ione, zt[d], *nd, zkt[d], *nd, queue[d] );
        }
        //  .. SpMV ..
        magma_setdevice(gpu_id);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue[0] );
        tic = MPI_Wtime();
        #endif
        c_hacapk_adot_body_lfmtx_batch_mgpu2(flag_set, zakt[0],st_leafmtxp,st_ctl, zkt,wws, zau_cpu1,wwr_cpu, 
                                             dBuffer,event, &time_batch,&time_set,&time_copy,
                                             &time_set1, &time_set2, &time_set3,
                                             on_gpu, queue, NULL, NULL);
        #if defined(PROF_MAGMA_BATCH)
        magma_queue_sync( queue[0] );
        time_spmv += (MPI_Wtime()-tic);
        #endif
        c_hacapk_adot_cax_lfmtx_comm_gpu(flag, zakt[0],zau_cpu1,
                                         st_ctl,buffer,disps, wws_cpu,wwr_cpu, isct,irct,*nd, 
                                         &time_copy,&time_mpi, queue[0]);
        #if defined(PROF_MAGMA_BATCH)
        tic = MPI_Wtime();
        #endif
        #if 0
        if (nrank == 1) {
            magma_setdevice(gpu_id);
            magma_dgetvector( *nd, zakt[0], 1, zau_cpu1, 1, queue[0] );
        }
        for (d=1; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_dsetvector_async( *nd, zau_cpu1, 1, zakt[d], 1, queue[d] );
        }
        #else
        magma_event_record( event[0], queue[0] );
        for (d=1; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_queue_wait_event( queue[d], event[0] );
            cudaMemcpyPeerAsync(zakt[d], gpu_id+d, zakt[0], gpu_id, (*nd)*sizeof(double),
                                magma_queue_get_cuda_stream( queue[d] ) );
            magma_event_record( event[d], queue[d] );
            magma_queue_wait_event( queue[0], event[d] );
        }
        #endif
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        toc = (MPI_Wtime()-tic);
        time_set  += toc;
        time_set4 += toc;

        tic = MPI_Wtime();
        #endif
        //
#if 0
        magma_setdevice(gpu_id);
        znorm = magma_ddot( *nd, zakt[0], ione, zt[0], ione, queue[0] ); 
        zden  = magma_ddot( *nd, zakt[0], ione, zakt[0], ione, queue[0] );
        zeta = znorm/zden;
#else
        offset = 0;
        for (dd=0; dd<gpus_per_proc; dd+=inc_dd) {
            d = (dd+dd0)%gpus_per_proc;
            int nd_d = (dd > swc_dd ? *nd-nd_loc2 : nd_loc2);
            magma_setdevice(gpu_id+d);
            #if defined(DDOT_BY_DGEMV)
            /*magma_dgemm( MagmaTrans, MagmaNoTrans,
                         2, 1, nd_d,
                         one, &(zt[d][offset]), *nd,
                              &(zakt[d][offset]), *nd,
                         zero,  znorm_d[d], 2, queue[d] );*/
            /*magmablas_dgemv( MagmaTrans, nd_d, 2,
                             one,  &(zt[d][offset]), *nd,
                                   &(zakt[d][offset]), ione,
                             zero, znorm_d[d], ione,
                             queue[d] );*/
            magma_dgemv( MagmaTrans, nd_d, 2,
                         one,  &(zt[d][offset]), *nd,
                               &(zakt[d][offset]), ione,
                         zero, znorm_d[d], ione,
                         queue[d] );
            #else
            cublasSetPointerMode( magma_queue_get_cublas_handle( queue[d] ), CUBLAS_POINTER_MODE_DEVICE );
            cublasDdot( magma_queue_get_cublas_handle( queue[d] ), nd_d, 
                        &(zakt[d][offset]), ione, &(zt[d][offset]), ione, &znorm_d[d][0] );
            cublasDdot( magma_queue_get_cublas_handle( queue[d] ), nd_d,
                        &(zakt[d][offset]), ione, &(zakt[d][offset]), ione, &znorm_d[d][1] );
            cublasSetPointerMode(magma_queue_get_cublas_handle( queue[d] ), CUBLAS_POINTER_MODE_HOST);
            #endif
            if (d == 1) { // GPU-1 -> GPU-3
                cudaMemcpyPeerAsync( &znorm_d[3][2], gpu_id+3, znorm_d[d], gpu_id+d, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[d] ) );
                magma_event_record( event[d], queue[d] );
            } else if (d == 2) { // GPU-2 -> GPU->0
                cudaMemcpyPeerAsync( &znorm_d[0][2], gpu_id+0, znorm_d[d], gpu_id+d, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[d] ) );
                magma_event_record( event[d], queue[d] );
            } else if (d == 3) {
                magma_queue_wait_event( queue[d], event[1] );
                magma_daxpy( 2, one,  &znorm_d[d][2], 1, &znorm_d[d][0], 1, queue[d] );

                // compute, copy it back, zeta = znorm/zden
                #if defined(DDOT_REDUCE_ON_CPU)
                magma_dgetvector( 2, znorm_d[3], 1, znorm_h[3], 1, queue[3] );
                znorm = znorm_h[3][0];
                zden  = znorm_h[3][1];
                zeta = znorm/zden;
                #else
                magmablas_dlacpy( MagmaFull, ione, ione, znorm_d[3], ione, zeta_d[3], ione, queue[3] );
                magma_dtrsm( MagmaRight, MagmaUpper, MagmaNoTrans, MagmaNonUnit,
                             ione, ione,
                             one, &znorm_d[3][1], ione,
                                   zeta_d[3], ione, queue[3] );
                cudaMemcpyPeerAsync( zeta_d[1], gpu_id+1, zeta_d[3], gpu_id+3, sizeof(double),
                                     magma_queue_get_cuda_stream( queue[3] ) );
                magma_event_record( event[3], queue[3] );
                magma_queue_wait_event( queue[1], event[3] );
                #endif
            } else if (d == 0) {
                magma_queue_wait_event( queue[d], event[2] );
                magma_daxpy( 2, one,  &znorm_d[d][2], 1, &znorm_d[d][0], 1, queue[d] );

                // compute, copy it back, zeta = znorm/zden
                #if defined(DDOT_REDUCE_ON_CPU)
                magma_dgetvector( 2, znorm_d[0], 1, znorm_h[0], 1, queue[0] );
                znorm = znorm_h[0][0];
                zden  = znorm_h[0][1];
                zeta = znorm/zden;
                #else
                magmablas_dlacpy( MagmaFull, ione, ione, znorm_d[0], ione, zeta_d[0], ione, queue[0] );
                magma_dtrsm( MagmaRight, MagmaUpper, MagmaNoTrans, MagmaNonUnit,
                             ione, ione,
                             one, &znorm_d[0][1], ione,
                                   zeta_d[0], ione, queue[0] );
                cudaMemcpyPeerAsync( zeta_d[2], gpu_id+2, zeta_d[0], gpu_id+0, sizeof(double),
                                     magma_queue_get_cuda_stream( queue[0] ) );
                magma_event_record( event[0], queue[0] );
                magma_queue_wait_event( queue[2], event[0] );
                #endif
            }
            if (dd == swc_dd) offset += nd_d;
        }
#endif
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        time_dot += (MPI_Wtime()-tic);
        #endif
        //
        // u(:nd) = u(:nd) + alpha*zkp(:nd) + zeta*zkt(:nd) (distributed)
#if 0
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magma_daxpy( *nd, alpha, zkp[d], ione, u[d], ione, queue[d] );
            magma_daxpy( *nd, zeta,  zkt[d], ione, u[d], ione, queue[d] );
        }
#else
        offset = 0;
        for (d=0; d<gpus_per_proc; d++) {
            int nd_d = (d == gpus_per_proc-1 ? *nd-d*nd_loc : nd_loc);
            magma_setdevice(gpu_id+d);
            #if defined(DDOT_REDUCE_ON_CPU)
            magma_daxpy( nd_d, alpha, &(zkp[d][offset]), ione, &(u[d][offset]), ione, queue[d] );
            magma_daxpy( nd_d, zeta,  &(zkt[d][offset]), ione, &(u[d][offset]), ione, queue[d] );
            #else
            device_daxpy( nd_d, alpha_d[d], &(zkp[d][offset]), ione, &(u[d][offset]), ione, &queue[d] );
            device_daxpy( nd_d, zeta_d[d],  &(zkt[d][offset]), ione, &(u[d][offset]), ione, &queue[d] );
            #endif
            offset += nd_d;
        }
#endif
        // zr(:nd) = zt(:nd) - zeta*zakt(:nd)
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            magmablas_dlacpy( MagmaFull, *nd, ione, zt[d], *nd, zr[d], *nd, queue[d] );
            #if defined(DDOT_REDUCE_ON_CPU)
            magma_daxpy( *nd, -zeta, zakt[d], ione, zr[d], ione, queue[d] );
            #else
            magmablas_dlascl( MagmaFull, ione, ione, -one, one, ione, ione, zeta_d[d], ione, queue[d], &info );
            device_daxpy( *nd, zeta_d[d], zakt[d], ione, zr[d], ione, &queue[d] );
            #endif
        }
        // beta = alpha/zeta * HACApK_dotp_d(nd,zshdw,zr)/znormold;
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        tic = MPI_Wtime();
        #endif
#if 0
        magma_setdevice(gpu_id);
        //beta   = magma_ddot( *nd, zshdw[0], ione, zr[0], ione, queue[0]);
        //zrnorm = magma_ddot( *nd, zr[0],    ione, zr[0], ione, queue[0] );
        d = (dd0+2)%gpus_per_proc;
        znorm_h[d][0] = magma_ddot( *nd, zshdw[0], ione, zr[0], ione, queue[0]);
        znorm_h[d][1] = magma_ddot( *nd, zr[0],    ione, zr[0], ione, queue[0] );
#else
        offset = 0;
        for (dd=0; dd<gpus_per_proc; dd+=inc_dd) {
            d = (dd0+dd)%gpus_per_proc;
            int nd_d = (dd > swc_dd ? *nd-nd_loc2 : nd_loc2);
            magma_setdevice(gpu_id+d);
            #if defined(DDOT_BY_DGEMV)
            /*magma_dgemm( MagmaTrans, MagmaNoTrans, 2, 1, nd_d,
                         one, &(zshdw[d][offset]), *nd,
                              &(zr[d][offset]), *nd,
                         zero,  znorm_d[d], 2, queue[d] );*/
            /*magmablas_dgemv( MagmaTrans, nd_d, 2,
                             one,  &(zshdw[d][offset]), *nd,
                                   &(zr[d][offset]), ione,
                             zero, znorm_d[d], ione,
                             queue[d] );*/
            magma_dgemv( MagmaTrans, nd_d, 2,
                         one,  &(zshdw[d][offset]), *nd,
                               &(zr[d][offset]), ione,
                         zero, znorm_d[d], ione,
                         queue[d] );
            #else
            cublasSetPointerMode( magma_queue_get_cublas_handle( queue[d] ), CUBLAS_POINTER_MODE_DEVICE );
            cublasDdot( magma_queue_get_cublas_handle( queue[d] ), nd_d, 
                        &(zr[d][offset]), ione, &(zshdw[d][offset]), ione, &znorm_d[d][0] );
            cublasDdot( magma_queue_get_cublas_handle( queue[d] ), nd_d,
                        &(zr[d][offset]), ione, &(zr[d][offset]), ione, &znorm_d[d][1] );
            cublasSetPointerMode(magma_queue_get_cublas_handle( queue[d] ), CUBLAS_POINTER_MODE_HOST);
            #endif
            if (d == 1) { // GPU-1 -> GPU-3
                cudaMemcpyPeerAsync( &znorm_d[3][2], gpu_id+3, znorm_d[d], gpu_id+d, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[d] ) );
                magma_event_record( event[d], queue[d] );
            } else if (d == 2) { // GPU-2 -> GPU->0
                cudaMemcpyPeerAsync( &znorm_d[0][2], gpu_id+0, znorm_d[d], gpu_id+d, 2*sizeof(double),
                                     magma_queue_get_cuda_stream( queue[d] ) );
                magma_event_record( event[d], queue[d] );
            } else if (d == 3) {
                magma_queue_wait_event( queue[d], event[1] );
                magma_daxpy( 2, one,  &znorm_d[d][2], 1, &znorm_d[d][0], 1, queue[d] );
                // for convergence check.
                magma_dgetvector_async( 2, znorm_d[d], 1, znorm_h[d], 1, queue[d] );
                #if !defined(DDOT_REDUCE_ON_CPU)
                // compute, copy it back, alpha=-znorm/zden
                cudaMemcpyPeerAsync( znorm_d[1], gpu_id+1, znorm_d[3], gpu_id+3, sizeof(double),
                                     magma_queue_get_cuda_stream( queue[3] ) );
                magma_event_record( event[3], queue[3] );
                magma_queue_wait_event( queue[1], event[3] );
                #endif
            } else if (d == 0) {
                magma_queue_wait_event( queue[d], event[2] );
                magma_daxpy( 2, one,  &znorm_d[d][2], 1, &znorm_d[d][0], 1, queue[d] );
                // compute, copy it back, alpha=-znorm/zden
                #if defined(DDOT_REDUCE_ON_CPU)
                magma_dgetvector_async( 2, znorm_d[d], 1, znorm_h[d], 1, queue[d] );
                #else
                cudaMemcpyPeerAsync( znorm_d[2], gpu_id+2, znorm_d[0], gpu_id+0, sizeof(double),
                                     magma_queue_get_cuda_stream( queue[0] ) );
                magma_event_record( event[0], queue[0] );
                magma_queue_wait_event( queue[2], event[0] );
                #endif
            }
            if (dd == swc_dd) offset += nd_d;
        }
#endif
        #if defined(PROF_MAGMA_BATCH)
        for (d=1; d<gpus_per_proc; d++) {
            magma_queue_sync( queue[d] );
        }
        time_dot += (MPI_Wtime()-tic);
        #endif
        //
        //beta *= (alpha/zeta) / znormold;
        #if defined(DDOT_REDUCE_ON_CPU)
        d = (dd0+2)%gpus_per_proc;
        magma_setdevice(gpu_id+d);
        magma_queue_sync( queue[d] );
        beta = znorm_h[d][0];
        beta = -alpha/zeta * beta/znormold;
        zrnorm = sqrt(znorm_h[d][1]);
        #else
        for (d=0; d<gpus_per_proc; d++) {
            magma_setdevice(gpu_id+d);
            // znormold = zeta/znormold
            magma_dgemm( MagmaNoTrans, MagmaNoTrans, 1, 1, 1,
                         one,   zeta_d[d], 1,
                               &alpha_d[d][1], 1,
                         zero, &alpha_d[d][0], 1, queue[d] );
            // beta = beta / znormold
            magmablas_dlacpy( MagmaFull, 1, 1, znorm_d[d], 1, beta_d[d], 1, queue[d] );
            magma_dtrsm( MagmaRight, MagmaUpper, MagmaNoTrans, MagmaNonUnit,
                         ione, ione,
                        -one, &alpha_d[d][0], ione,
                               beta_d[d], ione, queue[d] );
        }
        magma_setdevice(gpu_id+3);
        magma_queue_sync( queue[3] );
        zrnorm = sqrt(znorm_h[3][1]);
        #endif
        // from GPU-3
        *nstp = step;
        en_measure_time = MPI_Wtime();
        time = en_measure_time - st_measure_time;
        if (st_ctl->param[0] > 0 && mpinr == 0) {
            printf( " %d: time=%.2e log10(zrnorm/bnorm)=log10(%.2e/%.2e)=%.2e\n",step,time,zrnorm,bnorm,log10(zrnorm/bnorm) );
        }
    }
    //magma_dgetvector( *nd, u[0], 1, u_cpu, 1, queue[0] );
    int offset = 0;
    for (d=0; d<gpus_per_proc; d++) {
        int nd_d = (d == gpus_per_proc-1 ? *nd-d*nd_loc : nd_loc);
        magma_setdevice(gpu_id+d);
        magma_dgetvector( nd_d, &(u[d][offset]), 1, &(u_cpu[offset]), 1, queue[d] );
    }
    MPI_Barrier( icomm );
    en_measure_time = MPI_Wtime();
    time = en_measure_time - st_measure_time;
    if (st_ctl->param[0]>0) {
        //printf( " End: %d, %.2e\n",mpinr,time );
        if (mpinr == 0) {
            printf( "       BiCG       = %.5e\n", time );
            printf( "        time_mpi  = %.5e\n", time_mpi );
            printf( "        time_copy = %.5e\n", time_copy );
            printf( "        time_spmv = %.5e\n", time_spmv );
            printf( "        > time_batch = %.5e\n", time_batch );
            printf( "        > time_set   = %.5e\n", time_set );
            printf( "          + time_set1 = %.5e\n", time_set1 );
            printf( "          + time_set2 = %.5e\n", time_set2 );
            printf( "          + time_set3 = %.5e\n", time_set3 );
            printf( "          + time_set4 = %.5e\n", time_set4 );
            printf( "          +          += %.5e\n", time_set1+time_set2+time_set3+time_set4 );
            printf( "        > time_dot   = %.5e\n", time_dot );
            printf( "                     => %.5e\n", time_mpi+time_copy+time_spmv+time_dot );
        }
    }

    // free gpu memory
    for (d=0; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        magma_queue_sync( queue[d] );
        magma_queue_destroy( queue[d] );
        magma_queue_destroy( queue[d+gpus_per_proc] );

#if !defined(redundant_u)
        magma_free(u[d]);
#endif
        magma_free(b[d]);

        magma_free(zt[d]);
        magma_free(zp[d]);
        magma_free(zkp[d]);
        magma_free(zakp[d]);
        magma_free(zkt[d]);
        magma_free(zakt[d]);
        magma_free(zshdw[d]);
    }
    // delete matrix
    c_hacapk_adot_body_lfdel_mgpu_(st_leafmtxp);

    magma_setdevice(gpu_id);
    // free cpu memory
    free(queue);
    if (buffer != NULL) free(buffer);
    free(disps);
#if !defined(redundant_u)
    free(u);
#endif
    free(b);
    free(zt);
    free(zr);
    free(zp);
    free(zkp);
    free(zkt);
    free(zshdw);

    magma_free_pinned(zau_cpu1);
    magma_free_pinned(zau_cpu2);
    magma_free_pinned(wws_cpu);
    magma_free_pinned(wwr_cpu);

    magma_free(wws);
    magma_free(wwr);
    // setup peers
    for (d=1; d<gpus_per_proc; d++) {
        magma_setdevice(gpu_id+d);
        int dest;
        for (dest=0; dest<gpus_per_proc; dest++) {
            if (dest != d) cudaDeviceDisablePeerAccess( gpu_id+dest );
        }
    }
}
#endif
