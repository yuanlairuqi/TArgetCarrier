"""Shared BLE connection setup; retries never submit motion or edit Windows settings."""
import asyncio
import logging
import sys
import time
import threading
from types import SimpleNamespace
from bounded_gatt import bounded_await, close_session, cleanup_pending
from contextlib import asynccontextmanager

log = logging.getLogger('targetcarrier.link')
_setup_lock = threading.Lock()


@asynccontextmanager
async def radio_setup():
    # Both devices have their own asyncio owner. Serialize discovery/connection
    # setup across loops without blocking either loop or live notifications.
    while not _setup_lock.acquire(blocking=False): await asyncio.sleep(.05)
    try: yield
    finally: _setup_lock.release()


async def discover_devices(scanner, **kwargs):
    async with radio_setup(): return await scanner.discover(**kwargs)


def device_matches(device, advertisement, service, names):
    uuids=[u.lower() for u in (advertisement.service_uuids or [])]
    if service.lower() in uuids:return True
    # Some advertisements omit UUIDs or place the name in the scan response.
    # This is only a scan candidate; open_ble still validates the actual service.
    return (advertisement.local_name or device.name or '').startswith(tuple(names))


def connection_error(exc):
    code=getattr(exc,'winerror',None)
    if code is not None and code&0xffffffff==0x80070016:
        return 'Windows GATT 服务查询失败（0x80070016：设备不识别此命令）；尚未进入 STM32 握手'
    if isinstance(exc, (TimeoutError,asyncio.TimeoutError)):
        return 'BLE 连接/服务发现超时；尚未进入 STM32 握手'
    return f'{type(exc).__name__}: {exc}'


async def open_ble(factory, device, service, report, attempts=2, timeout=20, *, owner=None):
    """Pass the scanned BLEDevice to avoid implicit address discovery.

    Enumerate all GATT services, then strictly validate the expected service.
    Do not pass services=[UUID]: the UUID-filtered WinRT call returned
    0x80070016 on the user's adapter. Retry with the default cache policy if
    the uncached WinRT call itself is unsupported. Never write control data.
    """
    owner=owner if owner is not None else SimpleNamespace()
    if cleanup_pending(owner):
        raise RuntimeError("旧 GATT 操作尚未释放，禁止并发连接")
    for attempt in range(attempts):
        stage = 'BLE 连接与服务发现'
        started = time.monotonic()
        kwargs = dict(timeout=timeout)
        policy='全部服务 / 刷新缓存' if attempt==0 else '全部服务 / Windows 默认缓存策略'
        if sys.platform == 'win32' and attempt==0:
            kwargs['winrt'] = {'use_cached_services': False}
        client = factory(device, **kwargs)
        report(f'{stage}（{attempt+1}/{attempts}，{policy}）；连接中可取消')
        log.info('BLE connect begin service=%s device=%s attempt=%s discovery=all policy=%s', service, getattr(device, 'address', device), attempt+1,policy)
        try:
            async with radio_setup():
                await bounded_await(owner,client.connect(),timeout)
            report('GATT 服务枚举完成，正在核对设备控制服务')
            if client.services.get_service(service) is None:
                raise RuntimeError('服务 UUID 不匹配：可能选错设备或烧录了其他固件')
            log.info('BLE connect ready service=%s elapsed=%.2fs', service, time.monotonic()-started)
            return client
        except BaseException as exc:
            log.warning('BLE stage=%s service=%s elapsed=%.2fs exception=%r', stage, service, time.monotonic()-started, exc)
            await close_session(owner,client,None,2)
            if isinstance(exc, asyncio.CancelledError): raise
            if cleanup_pending(owner):
                raise RuntimeError("旧 GATT 操作尚未释放，连接尝试已停止；请等待设备断开或恢复后重试") from exc
            if isinstance(exc, RuntimeError) or attempt+1 >= attempts:
                if isinstance(exc, (TimeoutError, asyncio.TimeoutError)):
                    raise TimeoutError(f'{stage}超时（{timeout}秒/次）；尚未进入 STM32 握手。请关闭其他连接端、确认固件后重新搜索。') from exc
                raise
            report(connection_error(exc)+'；已释放旧会话，换兼容策略重试')
            await asyncio.sleep(.5)
    raise RuntimeError('BLE connection attempts exhausted')
