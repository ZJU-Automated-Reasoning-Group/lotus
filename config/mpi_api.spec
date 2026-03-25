# MPI API metadata.
# Format: SymbolOrPrefix TD_TYPE library=mpi semantic=<tag> [traits=<...>] [match=exact|prefix]

# MPI process management
MPI_Init TD_MPI_INIT library=mpi semantic=init
MPI_Init_thread TD_MPI_INIT library=mpi semantic=init-thread
MPI_Finalize TD_MPI_FINALIZE library=mpi semantic=finalize

# MPI Session Management (MPI-4.0)
MPI_Session_init TD_MPI_SESSION_INIT library=mpi semantic=session-init
MPI_Session_finalize TD_MPI_SESSION_FINALIZE library=mpi semantic=session-finalize
MPI_Session_get_info TD_MPI_SESSION_GET_INFO library=mpi semantic=session-get-info
MPI_Session_get_num_errcodes TD_MPI_SESSION_GET_NUM_ERRCODES library=mpi semantic=session-get-num-errcodes
MPI_Session_get_errhandler TD_MPI_SESSION_GET_ERRHANDLER library=mpi semantic=session-get-errhandler
MPI_Session_set_errhandler TD_MPI_SESSION_SET_ERRHANDLER library=mpi semantic=session-set-errhandler

# MPI Error Handling
MPI_Errhandler_create TD_MPI_ERRHANDLER_CREATE library=mpi semantic=errhandler-create
MPI_Errhandler_free TD_MPI_ERRHANDLER_FREE library=mpi semantic=errhandler-free
MPI_Comm_get_errhandler TD_MPI_COMM_GET_ERRHANDLER library=mpi semantic=comm-get-errhandler
MPI_Comm_set_errhandler TD_MPI_COMM_SET_ERRHANDLER library=mpi semantic=comm-set-errhandler
MPI_Comm_call_errhandler TD_MPI_COMM_CALL_ERRHANDLER library=mpi semantic=comm-call-errhandler
MPI_Win_get_errhandler TD_MPI_WIN_GET_ERRHANDLER library=mpi semantic=win-get-errhandler
MPI_Win_set_errhandler TD_MPI_WIN_SET_ERRHANDLER library=mpi semantic=win-set-errhandler
MPI_File_get_errhandler TD_MPI_FILE_GET_ERRHANDLER library=mpi semantic=file-get-errhandler
MPI_File_set_errhandler TD_MPI_FILE_SET_ERRHANDLER library=mpi semantic=file-set-errhandler
MPI_Error_class TD_MPI_ERROR_CLASS library=mpi semantic=error-class
MPI_Error_string TD_MPI_ERROR_STRING library=mpi semantic=error-string

# MPI Info Management
MPI_Info_create TD_MPI_INFO_CREATE library=mpi semantic=info-create
MPI_Info_dup TD_MPI_INFO_DUP library=mpi semantic=info-dup
MPI_Info_free TD_MPI_INFO_FREE library=mpi semantic=info-free
MPI_Info_get TD_MPI_INFO_GET library=mpi semantic=info-get
MPI_Info_get_valuelen TD_MPI_INFO_GET_VALUELEN library=mpi semantic=info-get-valuelen
MPI_Info_get_nkeys TD_MPI_INFO_GET_NKEYS library=mpi semantic=info-get-nkeys
MPI_Info_get_nthkey TD_MPI_INFO_GET_NTHKEY library=mpi semantic=info-get-nthkey
MPI_Info_get_keyval TD_MPI_INFO_GET_KEYVAL library=mpi semantic=info-get-keyval
MPI_Info_set TD_MPI_INFO_SET library=mpi semantic=info-set
MPI_Info_delete TD_MPI_INFO_DELETE library=mpi semantic=info-delete
MPI_Info_c2f TD_MPI_INFO_C2F library=mpi semantic=info-c2f
MPI_Info_create_env TD_MPI_INFO_CREATE_ENV library=mpi semantic=info-create-env
MPI_Info_free_env TD_MPI_INFO_FREE_ENV library=mpi semantic=info-free-env

# MPI Buffer Query Operations
MPI_Get_count TD_MPI_GET_COUNT library=mpi semantic=get-count
MPI_Get_elements TD_MPI_GET_ELEMENTS library=mpi semantic=get-elements
MPI_Get_elements_x TD_MPI_GET_ELEMENTS_X library=mpi semantic=get-elements-x
MPI_Status_size TD_MPI_STATUS_SIZE library=mpi semantic=status-size
MPI_Status_set_elements TD_MPI_STATUS_SET_ELEMENTS library=mpi semantic=status-set-elements
MPI_Status_set_elements_x TD_MPI_STATUS_SET_ELEMENTS_X library=mpi semantic=status-set-elements-x

# MPI Point-to-Point
MPI_Send TD_MPI_SEND library=mpi semantic=send
MPI_Ssend TD_MPI_SEND library=mpi semantic=ssend
MPI_Bsend TD_MPI_SEND library=mpi semantic=bsend
MPI_Rsend TD_MPI_SEND library=mpi semantic=rsend
MPI_Recv TD_MPI_RECV library=mpi semantic=recv
MPI_Sendrecv TD_MPI_SENDRECV library=mpi semantic=sendrecv
MPI_Sendrecv_replace TD_MPI_SENDRECV library=mpi semantic=sendrecv-replace
MPI_Probe TD_MPI_PROBE library=mpi semantic=probe

# MPI Message Matching (MPI-3.0)
MPI_Mprobe TD_MPI_MPROBE library=mpi semantic=mprobe
MPI_Improbe TD_MPI_IMPROBE library=mpi semantic=improbe
MPI_Imrecv TD_MPI_IMRECV library=mpi semantic=imrecv
MPI_Mrecv TD_MPI_MRECV library=mpi semantic=mrecv

# MPI Non-blocking Point-to-Point
MPI_Isend TD_MPI_ISEND library=mpi semantic=isend
MPI_Issend TD_MPI_ISEND library=mpi semantic=issend
MPI_Ibsend TD_MPI_ISEND library=mpi semantic=ibsend
MPI_Irsend TD_MPI_ISEND library=mpi semantic=irsend
MPI_Irecv TD_MPI_IRECV library=mpi semantic=irecv
MPI_Iprobe TD_MPI_IPROBE library=mpi semantic=iprobe

# MPI Wait/Test
MPI_Wait TD_MPI_WAIT library=mpi semantic=wait
MPI_Waitall TD_MPI_WAITALL library=mpi semantic=waitall
MPI_Waitany TD_MPI_WAITANY library=mpi semantic=waitany
MPI_Waitsome TD_MPI_WAITSOME library=mpi semantic=waitsome
MPI_Test TD_MPI_TEST library=mpi semantic=test
MPI_Testall TD_MPI_TESTALL library=mpi semantic=testall
MPI_Testany TD_MPI_TESTANY library=mpi semantic=testany
MPI_Testsome TD_MPI_TESTSOME library=mpi semantic=testsome

# MPI Collectives
MPI_Barrier TD_MPI_BARRIER library=mpi semantic=barrier traits=mpi-collective,mpi-barrier-blocking,mpi-collective-blocking
MPI_Ibarrier TD_MPI_BARRIER library=mpi semantic=ibarrier traits=mpi-collective,mpi-barrier-nonblocking,mpi-collective-nonblocking
MPI_Bcast TD_MPI_BCAST library=mpi semantic=bcast traits=mpi-collective,mpi-collective-blocking
MPI_Ibcast TD_MPI_BCAST library=mpi semantic=ibcast traits=mpi-collective,mpi-collective-nonblocking
MPI_Scatter TD_MPI_SCATTER library=mpi semantic=scatter traits=mpi-collective,mpi-collective-blocking
MPI_Scatterv TD_MPI_SCATTER library=mpi semantic=scatterv traits=mpi-collective,mpi-collective-blocking
MPI_Iscatter TD_MPI_SCATTER library=mpi semantic=iscatter traits=mpi-collective,mpi-collective-nonblocking
MPI_Iscatterv TD_MPI_SCATTER library=mpi semantic=iscatterv traits=mpi-collective,mpi-collective-nonblocking
MPI_Gather TD_MPI_GATHER library=mpi semantic=gather traits=mpi-collective,mpi-collective-blocking
MPI_Gatherv TD_MPI_GATHER library=mpi semantic=gatherv traits=mpi-collective,mpi-collective-blocking
MPI_Igather TD_MPI_GATHER library=mpi semantic=igather traits=mpi-collective,mpi-collective-nonblocking
MPI_Igatherv TD_MPI_GATHER library=mpi semantic=igatherv traits=mpi-collective,mpi-collective-nonblocking
MPI_Allgather TD_MPI_ALLGATHER library=mpi semantic=allgather traits=mpi-collective,mpi-collective-blocking
MPI_Allgatherv TD_MPI_ALLGATHER library=mpi semantic=allgatherv traits=mpi-collective,mpi-collective-blocking
MPI_Iallgather TD_MPI_ALLGATHER library=mpi semantic=iallgather traits=mpi-collective,mpi-collective-nonblocking
MPI_Iallgatherv TD_MPI_ALLGATHER library=mpi semantic=iallgatherv traits=mpi-collective,mpi-collective-nonblocking
MPI_Alltoall TD_MPI_ALLTOALL library=mpi semantic=alltoall traits=mpi-collective,mpi-collective-blocking
MPI_Alltoallv TD_MPI_ALLTOALL library=mpi semantic=alltoallv traits=mpi-collective,mpi-collective-blocking
MPI_Alltoallw TD_MPI_ALLTOALL library=mpi semantic=alltoallw traits=mpi-collective,mpi-collective-blocking
MPI_Ialltoall TD_MPI_ALLTOALL library=mpi semantic=ialltoall traits=mpi-collective,mpi-collective-nonblocking
MPI_Ialltoallv TD_MPI_ALLTOALL library=mpi semantic=ialltoallv traits=mpi-collective,mpi-collective-nonblocking
MPI_Ialltoallw TD_MPI_ALLTOALL library=mpi semantic=ialltoallw traits=mpi-collective,mpi-collective-nonblocking
MPI_Reduce TD_MPI_REDUCE library=mpi semantic=reduce traits=mpi-collective,mpi-collective-blocking
MPI_Ireduce TD_MPI_REDUCE library=mpi semantic=ireduce traits=mpi-collective,mpi-collective-nonblocking
MPI_Allreduce TD_MPI_ALLREDUCE library=mpi semantic=allreduce traits=mpi-collective,mpi-collective-blocking
MPI_Iallreduce TD_MPI_ALLREDUCE library=mpi semantic=iallreduce traits=mpi-collective,mpi-collective-nonblocking
MPI_Reduce_scatter TD_MPI_REDUCE_SCATTER library=mpi semantic=reduce-scatter traits=mpi-collective,mpi-collective-blocking
MPI_Reduce_scatter_block TD_MPI_REDUCE_SCATTER library=mpi semantic=reduce-scatter-block traits=mpi-collective,mpi-collective-blocking
MPI_Ireduce_scatter TD_MPI_REDUCE_SCATTER library=mpi semantic=ireduce-scatter traits=mpi-collective,mpi-collective-nonblocking
MPI_Ireduce_scatter_block TD_MPI_REDUCE_SCATTER library=mpi semantic=ireduce-scatter-block traits=mpi-collective,mpi-collective-nonblocking
MPI_Scan TD_MPI_SCAN library=mpi semantic=scan traits=mpi-collective,mpi-collective-blocking
MPI_Iscan TD_MPI_SCAN library=mpi semantic=iscan traits=mpi-collective,mpi-collective-nonblocking
MPI_Exscan TD_MPI_SCAN library=mpi semantic=exscan traits=mpi-collective,mpi-collective-blocking
MPI_Iexscan TD_MPI_SCAN library=mpi semantic=iexscan traits=mpi-collective,mpi-collective-nonblocking
MPI_Neighbor_allgather TD_MPI_ALLGATHER library=mpi semantic=neighbor-allgather traits=mpi-collective,mpi-collective-blocking
MPI_Neighbor_allgatherv TD_MPI_ALLGATHER library=mpi semantic=neighbor-allgatherv traits=mpi-collective,mpi-collective-blocking
MPI_Ineighbor_allgather TD_MPI_ALLGATHER library=mpi semantic=ineighbor-allgather traits=mpi-collective,mpi-collective-nonblocking
MPI_Ineighbor_allgatherv TD_MPI_ALLGATHER library=mpi semantic=ineighbor-allgatherv traits=mpi-collective,mpi-collective-nonblocking
MPI_Neighbor_alltoall TD_MPI_ALLTOALL library=mpi semantic=neighbor-alltoall traits=mpi-collective,mpi-collective-blocking
MPI_Neighbor_alltoallv TD_MPI_ALLTOALL library=mpi semantic=neighbor-alltoallv traits=mpi-collective,mpi-collective-blocking
MPI_Neighbor_alltoallw TD_MPI_ALLTOALL library=mpi semantic=neighbor-alltoallw traits=mpi-collective,mpi-collective-blocking
MPI_Ineighbor_alltoall TD_MPI_ALLTOALL library=mpi semantic=ineighbor-alltoall traits=mpi-collective,mpi-collective-nonblocking
MPI_Ineighbor_alltoallv TD_MPI_ALLTOALL library=mpi semantic=ineighbor-alltoallv traits=mpi-collective,mpi-collective-nonblocking
MPI_Ineighbor_alltoallw TD_MPI_ALLTOALL library=mpi semantic=ineighbor-alltoallw traits=mpi-collective,mpi-collective-nonblocking
MPI_Intercomm_bcast TD_MPI_BCAST library=mpi semantic=intercomm-bcast traits=mpi-collective,mpi-collective-blocking

# MPI RMA Window Management
MPI_Win_create TD_MPI_WIN_CREATE library=mpi semantic=win-create
MPI_Win_allocate TD_MPI_WIN_CREATE library=mpi semantic=win-allocate
MPI_Win_create_dynamic TD_MPI_WIN_CREATE library=mpi semantic=win-create-dynamic
MPI_Win_allocate_shared TD_MPI_WIN_CREATE library=mpi semantic=win-allocate-shared
MPI_Win_free TD_MPI_WIN_FREE library=mpi semantic=win-free

# MPI RMA Data Operations
MPI_Put TD_MPI_PUT library=mpi semantic=put
MPI_Rput TD_MPI_PUT library=mpi semantic=rput
MPI_Get TD_MPI_GET library=mpi semantic=get
MPI_Rget TD_MPI_GET library=mpi semantic=rget
MPI_Accumulate TD_MPI_ACCUMULATE library=mpi semantic=accumulate
MPI_Raccumulate TD_MPI_ACCUMULATE library=mpi semantic=raccumulate
MPI_Get_accumulate TD_MPI_ACCUMULATE library=mpi semantic=get-accumulate
MPI_Rget_accumulate TD_MPI_ACCUMULATE library=mpi semantic=rget-accumulate
MPI_Fetch_and_op TD_MPI_ACCUMULATE library=mpi semantic=fetch-and-op
MPI_Compare_and_swap TD_MPI_ACCUMULATE library=mpi semantic=compare-and-swap

# MPI RMA Synchronization
MPI_Win_fence TD_MPI_WIN_FENCE library=mpi semantic=win-fence
MPI_Win_lock TD_MPI_WIN_LOCK library=mpi semantic=win-lock
MPI_Win_lock_all TD_MPI_WIN_LOCK library=mpi semantic=win-lock-all
MPI_Win_unlock TD_MPI_WIN_UNLOCK library=mpi semantic=win-unlock
MPI_Win_unlock_all TD_MPI_WIN_UNLOCK library=mpi semantic=win-unlock-all
MPI_Win_flush TD_MPI_WIN_FLUSH library=mpi semantic=win-flush
MPI_Win_flush_all TD_MPI_WIN_FLUSH library=mpi semantic=win-flush-all
MPI_Win_flush_local TD_MPI_WIN_FLUSH library=mpi semantic=win-flush-local
MPI_Win_flush_local_all TD_MPI_WIN_FLUSH library=mpi semantic=win-flush-local-all
MPI_Win_sync TD_MPI_WIN_SYNC library=mpi semantic=win-sync

# MPI RMA PSCW Synchronization
MPI_Win_post TD_MPI_WIN_POST library=mpi semantic=win-post
MPI_Win_start TD_MPI_WIN_START library=mpi semantic=win-start
MPI_Win_complete TD_MPI_WIN_COMPLETE library=mpi semantic=win-complete
MPI_Win_wait TD_MPI_WIN_WAIT library=mpi semantic=win-wait
MPI_Win_test TD_MPI_WIN_TEST library=mpi semantic=win-test

# MPI Communicator Management
MPI_Comm_dup TD_MPI_COMM_DUP library=mpi semantic=comm-dup
MPI_Comm_dup_with_info TD_MPI_COMM_DUP library=mpi semantic=comm-dup-with-info
MPI_Comm_idup TD_MPI_COMM_DUP library=mpi semantic=comm-idup
MPI_Comm_split TD_MPI_COMM_SPLIT library=mpi semantic=comm-split
MPI_Comm_split_type TD_MPI_COMM_SPLIT library=mpi semantic=comm-split-type
MPI_Comm_create TD_MPI_COMM_CREATE library=mpi semantic=comm-create
MPI_Comm_create_group TD_MPI_COMM_CREATE library=mpi semantic=comm-create-group
MPI_Intercomm_create TD_MPI_COMM_CREATE library=mpi semantic=intercomm-create
MPI_Intercomm_create_from_groups TD_MPI_COMM_CREATE library=mpi semantic=intercomm-create-from-groups
MPI_Intercomm_merge TD_MPI_COMM_CREATE library=mpi semantic=intercomm-merge
MPI_Comm_free TD_MPI_COMM_FREE library=mpi semantic=comm-free
MPI_Comm_disconnect TD_MPI_COMM_FREE library=mpi semantic=comm-disconnect

# MPI Request Management
MPI_Request_free TD_MPI_REQUEST_FREE library=mpi semantic=request-free
MPI_Cancel TD_MPI_CANCEL library=mpi semantic=cancel

# MPI Datatype Management
MPI_Type_contiguous TD_MPI_TYPE_CONTIGUOUS library=mpi semantic=type-contiguous
MPI_Type_vector TD_MPI_TYPE_VECTOR library=mpi semantic=type-vector
MPI_Type_hvector TD_MPI_TYPE_HVECTOR library=mpi semantic=type-hvector
MPI_Type_indexed TD_MPI_TYPE_INDEXED library=mpi semantic=type-indexed
MPI_Type_hindexed TD_MPI_TYPE_HINDEXED library=mpi semantic=type-hindexed
MPI_Type_struct TD_MPI_TYPE_STRUCT library=mpi semantic=type-struct
MPI_Type_create_dlpack TD_MPI_TYPE_CREATE_DLPACK library=mpi semantic=type-create-dlpack
MPI_Type_create_subarray TD_MPI_TYPE_CREATE_SUBARRAY library=mpi semantic=type-create-subarray
MPI_Type_create_darray TD_MPI_TYPE_CREATE_DARRAY library=mpi semantic=type-create-darray
MPI_Type_create_resized TD_MPI_TYPE_CREATE_RESIZED library=mpi semantic=type-create-resized
MPI_Type_create_hindexed TD_MPI_TYPE_CREATE_HINDEXED library=mpi semantic=type-create-hindexed
MPI_Type_create_hvector TD_MPI_TYPE_CREATE_HVECTOR library=mpi semantic=type-create-hvector
MPI_Type_get_extent TD_MPI_TYPE_GET_EXTENT library=mpi semantic=type-get-extent
MPI_Type_get_true_extent TD_MPI_TYPE_GET_TRUE_EXTENT library=mpi semantic=type-get-true-extent
MPI_Type_size TD_MPI_TYPE_SIZE library=mpi semantic=type-size
MPI_Type_commit TD_MPI_TYPE_COMMIT library=mpi semantic=type-commit

# MPI Persistent Communications
MPI_Send_init TD_MPI_PERSISTENT_SEND_INIT library=mpi semantic=persistent-send-init traits=mpi-persistent-init
MPI_Ssend_init TD_MPI_PERSISTENT_SEND_INIT library=mpi semantic=persistent-ssend-init traits=mpi-persistent-init
MPI_Bsend_init TD_MPI_PERSISTENT_SEND_INIT library=mpi semantic=persistent-bsend-init traits=mpi-persistent-init
MPI_Rsend_init TD_MPI_PERSISTENT_SEND_INIT library=mpi semantic=persistent-rsend-init traits=mpi-persistent-init
MPI_Recv_init TD_MPI_PERSISTENT_RECV_INIT library=mpi semantic=persistent-recv-init traits=mpi-persistent-init
MPI_Start TD_MPI_REQUEST_START library=mpi semantic=start traits=mpi-request-start
MPI_Startall TD_MPI_REQUEST_START library=mpi semantic=startall traits=mpi-request-start

# MPI Process Topology (Cartesian)
MPI_Cart_create TD_MPI_CART_CREATE library=mpi semantic=cart-create
MPI_Cart_dims_create TD_MPI_CART_DIMS_CREATE library=mpi semantic=cart-dims-create
MPI_Cart_get TD_MPI_CART_GET library=mpi semantic=cart-get
MPI_Cart_shift TD_MPI_CART_SHIFT library=mpi semantic=cart-shift
MPI_Cart_coords TD_MPI_CART_COORDS library=mpi semantic=cart-coords
MPI_Cart_rank TD_MPI_CART_RANK library=mpi semantic=cart-rank
MPI_Cart_sub TD_MPI_CART_SUB library=mpi semantic=cart-sub

# MPI Process Topology (Distributed Graph - MPI-2.2+)
MPI_Dist_graph_create TD_MPI_DIST_GRAPH_CREATE library=mpi semantic=dist-graph-create
MPI_Dist_graph_create_adjacent TD_MPI_DIST_GRAPH_CREATE_ADJACENT library=mpi semantic=dist-graph-create-adjacent
MPI_Dist_graph_neighbors TD_MPI_DIST_GRAPH_NEIGHBORS library=mpi semantic=dist-graph-neighbors
MPI_Dist_graph_neighbors_count TD_MPI_DIST_GRAPH_NEIGHBORS_COUNT library=mpi semantic=dist-graph-neighbors-count

# MPI Process Topology (Legacy Graph - still used)
MPI_Graph_create TD_MPI_GRAPH_CREATE library=mpi semantic=graph-create
MPI_Graph_get TD_MPI_GRAPH_GET library=mpi semantic=graph-get
MPI_Graph_neighbors TD_MPI_GRAPH_NEIGHBORS library=mpi semantic=graph-neighbors
MPI_Graph_neighbors_count TD_MPI_GRAPH_NEIGHBORS_COUNT library=mpi semantic=graph-neighbors-count
MPI_Graphdims_get TD_MPI_GRAPH_DIMS_GET library=mpi semantic=graph-dims-get
MPI_Graph_map TD_MPI_GRAPH_MAP library=mpi semantic=graph-map
