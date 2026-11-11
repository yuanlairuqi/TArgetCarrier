"""Optional ESP historical first-fault snapshot. Never acknowledges or clears STOP."""
import struct
DIAGNOSTIC='6e400005-b5a3-f393-e0a9-e50e24dcca9e'
PEER_ERRORS=['正常','STM32 输入或指令错误','STM32 运动租约超时','STM32 未就绪','STM32 输出或发送异常']
def parse_diagnostic(data):
    b=bytes(data)
    if len(b)!=20 or b[0]!=1 or b[1]>10 or b[2]>9 or b[3]>4 or b[4]&~63 or b[5]>6 or b[6]>3 or b[7]>100:raise ValueError('ESP 故障快照格式错误')
    return dict(esp_error=b[1],bridge_error=b[2],stm_error=b[3],stm_message=PEER_ERRORS[b[3]],stm_flags=b[4],phase=b[5],motion=b[6],speed=b[7],uptime_ms=struct.unpack_from('<I',b,8)[0],loop_max_ms=struct.unpack_from('<I',b,12)[0],uart_age_ms=struct.unpack_from('<H',b,16)[0],notify_error=b[18],reset_reason=b[19],historical=True)
