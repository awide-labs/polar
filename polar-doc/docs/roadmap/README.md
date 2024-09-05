# Roadmap

Alibaba Cloud continuously releases updates to PolarDB PostgreSQL (hereafter simplified as PolarDB) to improve user experience. At present, Alibaba Cloud plans the following versions for PolarDB:

## Version 1.0

Version 1.0 supports shared storage and compute-storage separation. This version provides the minimum set of features such as Polar virtual file system (PolarVFS), flushing and buffer management, LogIndex, and SyncDDL.

- PolarVFS: A VFS is abstracted from the database engine. This way, the database engine can connect to all types of storage, and you do not need to consider whether the storage uses buffered I/O or direct I/O.
- Flushing and buffer management: In each PolarDB cluster, data is separately processed on each compute node, but all compute nodes share the same physical storage. The speed at which the primary node flushes write-ahead logging (WAL) records must be controlled to prevent the read-only nodes from reading future pages.
- LogIndex: The read-only nodes cannot flush WAL records. When you query a page on a read-only node, the read-only node reads a previous version of the page from the shared storage. Then, the read-only node reads and replays the WAL records of the page from its memory to obtain the most recent version of the page. Each LogIndex record consists of the metadata of a specific WAL record. The read-only nodes can efficiently retrieve the WAL records of a page by using LogIndex records.
- SyncDDL: PolarDB supports compute-storage separation. When the primary node runs DDL operations, it considers the objects, such as relations, that are referenced by the read-only nodes. The locks that are held by the DDL operations are synchronized from the primary node to the read-only nodes.
- db-monitor: The db-monitor module monitors the host on which your PolarDB cluster runs. The db-monitor module also monitors the databases that you create in your PolarDB cluster. The monitoring data provides a basis for switchovers and helps ensure high availability.

## Version 2.0

In addition to improvements to compute-storage separation, version 2.0 provides a significantly improved optimizer.

- UniqueKey: The UniqueKey module ensures that the data on plan nodes is unique. This feature is similar to the ordering feature that you can use on plan nodes. Data uniqueness reduces unnecessary DISTINCT and GROUP BY clauses and improves the ordering of the results of joins.

## Version 3.0

The availability of PolarDB with compute-storage separation is significantly improved.

- Parallel replay: LogIndex enables PolarDB to replay WAL records in lazy replay mode. In the lazy replay mode, the read-only nodes only mark the WAL records of each updated page. The read-only nodes read and replay the WAL records only when you query the page on these nodes. The lazy replay mechanism may impair read performance. Version 3.0 uses the parallel replay mechanism together with the lazy replay mechanism to accelerate read queries.
- OnlinePromote: If the primary node unexpectedly exits, your workloads can be switched over to a read-only node. The read-only node does not need to restart. The read-only node is promoted to run as the new primary node immediately after it replays all WAL records in parallel. This significantly reduces downtime.

## Version 4.0

Version 4.0 can meet your growing business requirements in hybrid transaction/analytical processing (HTAP) scenarios. Version 4.0 is based on the shared storage-based massively parallel processing (MPP) architecture, which allows PolarDB to fully utilize the CPU, memory, and I/O resources of multiple read-only nodes.

Test results show that the performance of a PolarDB cluster linearly increases as you increase the number of cores from 1 to 256.

## Version 5.0

In earlier versions, each PolarDB cluster consists of one primary node that processes both read requests and write requests and one or more read-only nodes that process only read requests. You can increase the read capability of a PolarDB cluster by creating more read-only nodes. However, you cannot increase the writing capability because each PolarDB cluster consists of only one primary node.

Version 5.0 uses the shared-nothing architecture together with the shared-everything architecture. This allows multiple compute nodes to process write requests.
