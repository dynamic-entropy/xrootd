# HTTPS Third Party Copy for XRootD

The `xrootd-tpc` module provides an implementation of HTTPS third-party-copy
for the HTTPS implementation inside XRootD.

To enable, set the following in the configuration file:

```
http.exthandler xrdtpc libXrdHttpTPC.so
```

## Transfer worker pool

Single-stream transfers do not run on the thread that received the `COPY`.
They are handed to a pool of worker threads.  The thread that received the
`COPY` stays with the client: it sends the performance markers and the final
status.  The queue lock is not held while libcurl runs.

### Which transfers share a queue

A queue is one remote endpoint plus the identity of the client that asked for
the copy:

- scheme, host, and port (the path is not part of the key)
- SciTag, when the client sent one (`0` is a real tag and is not the same as
  an absent tag)
- virtual organization
- local address bound with `tpc.fixed_route`, when it could be determined
- a hash of the outgoing credential: `TransferHeader*` and `Copy-Header`
  values, URL userinfo, the X.509 name, and the raw credential bytes

The hash is what is logged.  The token or certificate is not.  Two transfers
share a worker only when every one of those matches, so a cached connection
is never handed to a different host, token, certificate, SciTag, or source
address.

### Connection reuse

Each worker keeps the connection it opened after a transfer that finished
cleanly, and the next transfer on that queue is given to the worker that
became idle most recently.  The worker is marked idle before the client is
told the previous transfer finished, so a pull `HEAD` and the `GET` that
follows land on the same connection.

A redirect response closes its own connection while it is handled, so a
pull through a redirector does not keep that socket open for the rest of
the transfer.  A transfer that fails, is cancelled, is redirected, or ends
with an HTTP error also drops that worker's connection cache before the
next request starts.
Per-transfer options (headers, ranges, timeouts, `Expect`) live on the easy
handle, which is not reused; the `HEAD` probe clears the options it set
before the transfer runs.

A remote server may close an idle connection before the worker gives it up.
libcurl then opens a new connection for the next transfer.

Multi-stream transfers (`X-Number-Of-Streams` greater than 1) do not use the
pool and do not count against `tpc.max_active_transfers` or the per-link cap.
They already run on the client thread, with one connection per stream.
Putting them in the pool would queue those ranges behind the per-link cap.
A redirect hop is still closed, and the data connection can be reused by
the next range on that stream.  The pool caps bound pooled workers only.

### Limits and fairness

Each queue runs at most `tpc.max_active_transfers_per_queue` workers.  Further
transfers for that same link wait on that queue.  A slow transfer therefore
does not occupy a worker that belongs to a different endpoint, and transfers
to distinct endpoints are not limited by this number.

The whole pool runs at most `tpc.max_active_transfers` workers.  Past that, a
transfer still waits on its own queue; it is not rejected and it is not lined
up behind some other link's transfers.  A link that has work and no worker is
given the next free slot.  An idle worker is closed to free a slot, and a
worker that finishes while some other link is waiting exits when this link
still has another worker or has nothing left queued.  A link that has only
one worker keeps it, so a busy link is not frozen to feed a new one.

A waiting transfer gets performance markers, and the time it spends waiting
does not count towards `tpc.timeout`.  A transfer cancelled while it waits,
for example because the client disconnected, is removed from the queue.  A
running transfer is cancelled by waking its worker, so it does not sit until
the next one-second curl wait.

When `tpc.max_waiting_transfers_per_queue` transfers for one link are already
waiting, further ones for that link are rejected with `429 Too Many Requests`.

### Configuration

```
tpc.max_active_transfers <n>
tpc.max_active_transfers_per_queue <n>
tpc.max_waiting_transfers_per_queue <n>
tpc.worker_idle_timeout <time>
```

| Directive | Default | Meaning |
|---|---|---|
| `tpc.max_active_transfers` | 4096 | Workers in the whole pool.  Above this, transfers wait on their own queue. |
| `tpc.max_active_transfers_per_queue` | 20 | Workers for one link.  Distinct links each get this many. |
| `tpc.max_waiting_transfers_per_queue` | 20 | Transfers waiting on one link before new ones are rejected. |
| `tpc.worker_idle_timeout` | 60s | How long an idle worker, and the connection it holds, are kept when no other link is waiting for a slot. |

`tpc.max_active_transfers` is a backstop against one thread per remote host,
not a throttle for ordinary load: the thread that accepted the `COPY` is
still occupied for the whole transfer.  Raise it only if a host really runs
more concurrent single-stream copies than the default.
`tpc.worker_idle_timeout` takes a time such as `90`, `90s` or `5m`, from 1
second to 1 hour.  A longer timeout keeps connections for longer between bursts,
at the cost of idle threads and sockets.

With `tpc.trace info` the server logs when each worker starts and exits, and
the queue name of each new link.


## HTTPS TPC technical details.

Third-party-copy is done using the standard WebDAV copy verb.  The actors involved are:

1.  The client orchestrating the transfer.
2.  The source server where the resource is read from.
3.  The destination server where the resource is written to.

The client initiates the third party copy by issuing a COPY request to _either_ the source or destination
endpoint.  Typically, when this is done, the initial endpoint redirects the client to a second service
(the disk server) that will performs the actual transfer.

When the client contacts the source server, it should set the `Destination` header so the source knows
where to put the data.  Analogously, if the client first contacts the destination server, it should set
the `Source` header.

For the former case, the interaction looks like this:

```
-> COPY /sfn/of/source/replica HTTP/1.1
   Destination: https://<dest-endpoint>/pfn/of/dest/replica
<- HTTP/1.1 302 Found
   Location: https://<source-disk-server>/pfn/of/source/replica
```

where `<source-disk-server>` is the service that will actually perform the transfer.

The client would follow the redirection and retry the `COPY`:

```
-> COPY /sfn/of/source/replica HTTP/1.1
   Destination: https://<dest-endpoint>/pfn/of/dest/replica
<- HTTP/1.1 201 Created
   Transfer-Encoding: chunked
```

The source server *should* respond with an appropriate status code (such as 201) in a timely manner.
As a TPC can take a significant amount of time, the source server SHOULD NOT wait until the transfer is
finished before responding with a success.  In the case when a transfer can be started, it is recommended
that the response be started as soon as possible.

In the case of a transfer being started (or queued) by the source server, it SHOULD use chunked encoding
and send a multipart response.

Next `<source-disk-server>` will connect to the destination endpoint and actually perform the `PUT`:

```
-> PUT /pfn/of/destination/replica HTTP/1.1
<- HTTP/1.1 201 Created
```

As the PUT is ongoing, the source disk server should send back a periodic transfer chunk of the following
form:

```
Perf Marker
	Timestamp: 1360578938
	Stripe Index: 0
	Stripe Bytes Transferred: 49397760
	Total Stripe Count: 1
End
```

Timestamps should be seconds from Unix epoch.  It is recommended that the time period between chunks be
less than 30 seconds.

If the transfer ultimately succeeds, then the last chunk should be of the following form:

```
success: Created
```

Here, `success` indicates the transfer status: subsequent text is informational for the user.

Failure of the transfer can be indicated with the prefix `failed` or `failure`:

```
failure: Network connection unexpectedly closed.
```

Finally, if the source disk server decides to cancel or abort the transfer, use the `aborted` prefix:

```
aborted: Transfer took too long.
```

There is an analogous case when the client contacts the destination server to perform the `COPY` and
sets the `Source` header.  In such a case, the response to the client looks identical but the destination
server will perform a `GET` instead of a `PUT`.

In some cases, the user may want the server performing the transfer to append additional headers
(such as an HTTP `Authorization` header) to the transfer it initiates.  In such case, the client should
utilize the `TransferHeader` mechanism.  Add a header of the form:

```
TransferHeader<header> : <value>
```

For example, if the client sends the following to the source server as part of its `COPY` request:

```
TransferHeaderAuthorization: Basic cGF1bDpwYXNzd29yZA==
```

then the source server should set the following header as part of its `PUT` request to the destination:

```
Authorization: Basic cGF1bDpwYXNzd29yZA==
```
