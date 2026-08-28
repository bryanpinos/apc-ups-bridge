#!/usr/bin/env python3
"""Parse a USB HID report descriptor and emit the field map (report id, bit offset, size, usage)."""
import sys

PWR = {0x01:'iName',0x02:'PresentStatus',0x03:'ChangedStatus',0x04:'UPS',0x05:'PowerSupply',
 0x10:'BatterySystem',0x11:'BatterySystemID',0x12:'Battery',0x13:'BatteryID',0x14:'Charger',
 0x15:'ChargerID',0x16:'PowerConverter',0x17:'PowerConverterID',0x18:'OutletSystem',
 0x19:'OutletSystemID',0x1A:'Input',0x1B:'InputID',0x1C:'Output',0x1D:'OutputID',0x1E:'Flow',
 0x1F:'FlowID',0x20:'Outlet',0x21:'OutletID',0x22:'Gang',0x23:'GangID',0x24:'PowerSummary',
 0x25:'PowerSummaryID',0x30:'Voltage',0x31:'Current',0x32:'Frequency',0x33:'ApparentPower',
 0x34:'ActivePower',0x35:'PercentLoad',0x36:'Temperature',0x37:'Humidity',0x38:'BadCount',
 0x40:'ConfigVoltage',0x41:'ConfigCurrent',0x42:'ConfigFrequency',0x43:'ConfigApparentPower',
 0x44:'ConfigActivePower',0x45:'ConfigPercentLoad',0x46:'ConfigTemperature',0x47:'ConfigHumidity',
 0x50:'SwitchOnControl',0x51:'SwitchOffControl',0x52:'ToggleControl',0x53:'LowVoltageTransfer',
 0x54:'HighVoltageTransfer',0x55:'DelayBeforeReboot',0x56:'DelayBeforeStartup',
 0x57:'DelayBeforeShutdown',0x58:'Test',0x59:'ModuleReset',0x5A:'AudibleAlarmControl',
 0x60:'Present',0x61:'Good',0x62:'InternalFailure',0x63:'VoltageOutOfRange',
 0x64:'FrequencyOutOfRange',0x65:'Overload',0x66:'OverCharged',0x67:'OverTemperature',
 0x68:'ShutdownRequested',0x69:'ShutdownImminent',0x6B:'SwitchOnOff',0x6C:'Switchable',
 0x6D:'Used',0x6E:'Boost',0x6F:'Buck',0x70:'Initialized',0x71:'Tested',0x72:'AwaitingPower',
 0x73:'CommunicationLost',0xFD:'iManufacturer',0xFE:'iProduct',0xFF:'iSerialNumber'}

BAT = {0x01:'SMBBatteryMode',0x02:'SMBBatteryStatus',0x03:'SMBAlarmWarning',0x28:'ManufacturerAccess',
 0x29:'RemainingCapacityLimit',0x2A:'RemainingTimeLimit',0x2B:'AtRate',0x2C:'CapacityMode',
 0x2D:'BroadcastToCharger',0x2E:'PrimaryBattery',0x2F:'ChargeController',0x40:'TerminateCharge',
 0x41:'TerminateDischarge',0x42:'BelowRemainingCapacityLimit',0x43:'RemainingTimeLimitExpired',
 0x44:'Charging',0x45:'Discharging',0x46:'FullyCharged',0x47:'FullyDischarged',
 0x48:'ConditioningFlag',0x49:'AtRateOK',0x4A:'SMBErrorCode',0x4B:'NeedReplacement',
 0x60:'AtRateTimeToFull',0x61:'AtRateTimeToEmpty',0x62:'AverageCurrent',0x63:'MaxError',
 0x64:'RelativeStateOfCharge',0x65:'AbsoluteStateOfCharge',0x66:'RemainingCapacity',
 0x67:'FullChargeCapacity',0x68:'RunTimeToEmpty',0x69:'AverageTimeToEmpty',0x6A:'AverageTimeToFull',
 0x6B:'CycleCount',0x83:'DesignCapacity',0x85:'ManufacturerDate',0x86:'SerialNumber',
 0x87:'iManufacturerName',0x88:'iDeviceName',0x89:'iDeviceChemistry',0x8A:'ManufacturerData',
 0x8B:'Rechargeable',0x8C:'WarningCapacityLimit',0x8D:'CapacityGranularity1',
 0x8E:'CapacityGranularity2',0x8F:'iOEMInformation',0xC0:'InhibitCharge',0xC1:'EnablePolling',
 0xC2:'ResetToZero',0xD0:'ACPresent',0xD1:'BatteryPresent',0xD2:'PowerFail',0xD3:'AlarmInhibited',
 0xD4:'ThermistorUnderRange',0xD5:'ThermistorHot',0xD6:'ThermistorCold',0xD7:'ThermistorOverRange',
 0xD8:'VoltageOutOfRange',0xD9:'CurrentOutOfRange',0xDA:'CurrentNotRegulated',
 0xDB:'VoltageNotRegulated',0xDC:'MasterMode'}

def uname(pg, us):
    if pg == 0x84: return PWR.get(us, '0x84:%02X' % us)
    if pg == 0x85: return BAT.get(us, '0x85:%02X' % us)
    if pg == 0xFF86: return 'APC:0x%02X' % us
    return '%04X:%02X' % (pg, us)

data = bytes.fromhex(open(sys.argv[1]).read().strip())
i = 0
g = {'usage_page':0,'report_size':0,'report_count':0,'report_id':0,
     'logical_min':0,'logical_max':0,'unit':0,'unit_exp':0}
stack = []
locals_usages = []
bitpos = {}          # (report_id, kind) -> next free bit
fields = []
TYPE = {0:'Main',1:'Global',2:'Local'}

while i < len(data):
    b = data[i]; i += 1
    size = b & 0x03
    size = 4 if size == 3 else size
    typ  = (b >> 2) & 0x03
    tag  = (b >> 4) & 0x0F
    val = 0
    for k in range(size):
        val |= data[i+k] << (8*k)
    i += size

    if typ == 1:   # Global
        if   tag == 0x0: g['usage_page'] = val
        elif tag == 0x1: g['logical_min'] = val
        elif tag == 0x2: g['logical_max'] = val
        elif tag == 0x5: g['unit_exp'] = val
        elif tag == 0x6: g['unit'] = val
        elif tag == 0x7: g['report_size'] = val
        elif tag == 0x8: g['report_id'] = val
        elif tag == 0x9: g['report_count'] = val
        elif tag == 0xA: stack.append(dict(g))
        elif tag == 0xB:
            if stack: g = stack.pop()
    elif typ == 2:  # Local
        if tag == 0x0:
            # 4-byte usage carries the page in the high word
            if size == 4: locals_usages.append((val >> 16, val & 0xFFFF))
            else:         locals_usages.append((g['usage_page'], val))
    elif typ == 0:  # Main
        if tag in (0x8, 0x9, 0xB):
            kind = {0x8:'Input', 0x9:'Output', 0xB:'Feature'}[tag]
            rid = g['report_id']
            key = (rid, kind)
            off = bitpos.get(key, 0)
            cnt, sz = g['report_count'], g['report_size']
            for n in range(cnt):
                if n < len(locals_usages): pg, us = locals_usages[n]
                elif locals_usages:        pg, us = locals_usages[-1]
                else:                      pg, us = g['usage_page'], 0
                fields.append(dict(rid=rid, kind=kind, bit=off + n*sz, size=sz,
                                   page=pg, usage=us,
                                   lmin=g['logical_min'], lmax=g['logical_max'],
                                   uexp=g['unit_exp'], const=bool(val & 0x01)))
            bitpos[key] = off + cnt*sz
            locals_usages = []
        elif tag == 0xA:  # Collection
            locals_usages = []
        elif tag == 0xC:  # End collection
            locals_usages = []

feat = [f for f in fields if f['kind'] == 'Feature' and not f['const'] and f['usage']]
print('%d total fields, %d usable Feature fields\n' % (len(fields), len(feat)))
print('%-4s %-6s %-5s %-30s %s' % ('Rpt','bit','bits','usage','logical range'))
print('-'*78)
for f in sorted(feat, key=lambda x: (x['rid'], x['bit'])):
    exp = f['uexp'] - 16 if f['uexp'] > 7 else f['uexp']
    scale = ('  x10^%d' % exp) if exp else ''
    print('%-4d %-6d %-5d %-30s [%d..%d]%s' % (
        f['rid'], f['bit'], f['size'], uname(f['page'], f['usage']),
        f['lmin'], f['lmax'], scale))
