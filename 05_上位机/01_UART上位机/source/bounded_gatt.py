"""Bounded application teardown; never interpret cancel() as native completion."""
import asyncio
import threading
_registry_lock=threading.RLock()
class WriteChannelTimeout(TimeoutError):pass

def retire(owner, task):
    with _registry_lock:
        retired=getattr(owner,'_gatt_retired',None)
        if retired is None:retired=owner._gatt_retired=set()
        retired.add(task)
    def finished(t):
        with _registry_lock:retired.discard(t)
        if not t.cancelled():t.exception()  # Consume failures even after the session is gone.
    task.add_done_callback(finished)
    return task

def cleanup_pending(owner):
    with _registry_lock:
        clients=tuple(getattr(owner,'_gatt_unreleased_clients',()))
        tasks=tuple(getattr(owner,'_gatt_retired',()))
    def connected(c):
        try:return bool(c.is_connected)
        except Exception:return True
    released={id(c) for c in clients if not connected(c)}
    with _registry_lock:
        live=getattr(owner,'_gatt_unreleased_clients',[])
        live[:]=[c for c in live if id(c) not in released]
        have_clients=bool(live)
        tasks=tuple(getattr(owner,'_gatt_retired',()))
    return bool(have_clients or any(not t.done() for t in tasks))

async def bounded_await(owner,awaitable,timeout):
    task=asyncio.ensure_future(awaitable)
    try:
        done,_=await asyncio.wait([task],timeout=timeout)
        if done:return task.result()
    except asyncio.CancelledError:
        retire(owner,task);task.cancel();raise
    retire(owner,task);task.cancel()
    raise WriteChannelTimeout('GATT operation deadline exceeded; native completion unknown')

async def close_session(owner,client,writer,timeout):
    tasks=[]
    if writer is not None:
        tasks.append(retire(owner,writer))
        if not writer.done():writer.cancel()
    if client is not None:
        with _registry_lock:
            clients=getattr(owner,'_gatt_unreleased_clients',None)
            if clients is None:clients=owner._gatt_unreleased_clients=[]
            if not any(c is client for c in clients):clients.append(client)
        async def disconnect():await client.disconnect()
        tasks.append(retire(owner,asyncio.create_task(disconnect())))
    if not tasks:return not cleanup_pending(owner)
    try:
        _,pending=await asyncio.wait(tasks,timeout=timeout)
    finally:
        # The registry keeps native operations fenced until they actually finish.
        for task in tasks:
            if not task.done():task.cancel()

    return not cleanup_pending(owner)
