#include "HCISocket.h"

namespace WPEFramework {

namespace Bluetooth {

uint32_t HCISocket::Advertising(const bool enable, const uint8_t mode)
{
    uint32_t result = Core::ERROR_ILLEGAL_STATE;

    if (enable == true) {
        if (IsAdvertising() == false) {
            result = Core::ERROR_BAD_REQUEST;
            Command::AdvertisingParametersLE parameters;

            parameters.Clear();
            parameters->min_interval = htobs(0x0800);
            parameters->max_interval = htobs(0x0800);
            parameters->advtype = mode;
            parameters->chan_map = 7;

            if (Exchange(MAX_ACTION_TIMEOUT, parameters, parameters) == Core::ERROR_NONE) {
            // if ((temp == Core::ERROR_NONE) && (parameters.Response() == 0)) {

                Command::AdvertisingEnableLE advertising;

                advertising.Clear();
                advertising->enable = 1;

                if (Exchange(MAX_ACTION_TIMEOUT, advertising, advertising) == Core::ERROR_NONE) { //  && (advertising.Response() == 0)) {
                    printf ("Status = %d\n", advertising.Response());
                    _state.SetState(static_cast<state>(_state.GetState() | ADVERTISING));
                    result = Core::ERROR_NONE;
                }
            }
        }
    } else if (IsAdvertising() == true) {
        result = Core::ERROR_BAD_REQUEST;
        Command::AdvertisingEnableLE advertising;

        advertising.Clear();
        advertising->enable = 0;

        if ((Exchange(MAX_ACTION_TIMEOUT, advertising, advertising) == Core::ERROR_NONE) && (advertising.Response() == 0)) {
            _state.SetState(static_cast<state>(_state.GetState() & (~ADVERTISING)));
            result = Core::ERROR_NONE;
        }
    }
    return (result);
}

void HCISocket::Scan(const uint16_t scanTime, const uint32_t type, const uint8_t flags)
{
    ASSERT(scanTime <= 326);

    _state.Lock();

    if ((_state & ACTION_MASK) == 0) {
        int descriptor = Handle();

        _state.SetState(static_cast<state>(_state.GetState() | SCANNING));

        _state.Unlock();

        ASSERT(descriptor >= 0);

        void* buf = ALLOCA(sizeof(struct hci_inquiry_req) + (sizeof(inquiry_info) * 128));
        struct hci_inquiry_req* ir = reinterpret_cast<struct hci_inquiry_req*>(buf);
        std::list<Address> reported;

        ir->dev_id = hci_get_route(nullptr);
        ir->num_rsp = 128;
        ir->length = (((scanTime * 100) + 50) / 128);
        ir->flags = flags | IREQ_CACHE_FLUSH;
        ir->lap[0] = (type >> 16) & 0xFF; // 0x33;
        ir->lap[1] = (type >> 8) & 0xFF; // 0x8b;
        ir->lap[2] = type & 0xFF; // 0x9e;
        // Core::Time endTime = Core::Time::Now().Add(scanTime * 1000);

        // while ((ir->length != 0) && (ioctl(descriptor, HCIINQUIRY, reinterpret_cast<unsigned long>(buf)) >= 0)) {
            if (ioctl(descriptor, HCIINQUIRY, reinterpret_cast<unsigned long>(buf)) >= 0) {

            for (uint8_t index = 0; index < (ir->num_rsp); index++) {
                inquiry_info* info = reinterpret_cast<inquiry_info*>(&(reinterpret_cast<uint8_t*>(buf)[sizeof(hci_inquiry_req)]));

                bdaddr_t* address = &(info[index].bdaddr);
                Address newSource(*address);

                std::list<Address>::const_iterator finder(std::find(reported.begin(), reported.end(), newSource));

                if (finder == reported.end()) {
                    reported.push_back(newSource);
                    Discovered(false, newSource, _T("[Unknown]"));
                }
            }

            // Reset go for the next round !!
            // ir->length  = (endTime <= Core::Time::Now() ? 0 : 1);
            // ir->num_rsp = 128;
            // ir->flags  &= ~IREQ_CACHE_FLUSH;
        }

        _state.Lock();

        _state.SetState(static_cast<state>(_state.GetState() & (~(ABORT | SCANNING))));
    }

    _state.Unlock();
}

void HCISocket::Scan(const uint16_t scanTime, const bool limited, const bool passive)
{
    _state.Lock();

    if ((_state & ACTION_MASK) == 0) {
        Command::ScanParametersLE parameters;

        parameters.Clear();
        parameters->type = (passive ? 0x00 : 0x01);
        parameters->interval = htobs((limited ? 0x12 : 0x10));
        parameters->window = htobs((limited ? 0x12 : 0x10));
        parameters->own_bdaddr_type = LE_PUBLIC_ADDRESS;
        parameters->filter = SCAN_FILTER_POLICY;

        if ((Exchange(MAX_ACTION_TIMEOUT, parameters, parameters) == Core::ERROR_NONE) && (parameters.Response() == 0)) {

            Command::ScanEnableLE scanner;
            scanner.Clear();
            scanner->enable = 1;
            scanner->filter_dup = SCAN_FILTER_DUPLICATES;

            if ((Exchange(MAX_ACTION_TIMEOUT, scanner, scanner) == Core::ERROR_NONE) && (scanner.Response() == 0)) {

                _state.SetState(static_cast<state>(_state.GetState() | SCANNING));

                // Now lets wait for the scanning period..
                _state.Unlock();

                _state.WaitState(ABORT, scanTime * 1000);

                _state.Lock();

                scanner->enable = 0;
                Exchange(MAX_ACTION_TIMEOUT, scanner, scanner);

                _state.SetState(static_cast<state>(_state.GetState() & (~(ABORT | SCANNING))));
            }
        }
    }

    _state.Unlock();
}

void HCISocket::Abort()
{
    _state.Lock();
            
    if ((_state & ACTION_MASK) != 0) {
        // TODO: Find if we can actually abort a IOCTL:HCIINQUIRY !!
        _state.SetState(static_cast<state>(_state.GetState() | ABORT));
    }

    _state.Unlock();
}

/* virtual */ void HCISocket::StateChange() 
{
    Core::SynchronousChannelType<Core::SocketPort>::StateChange();
    if (IsOpen() == true) {
        hci_filter_clear(&_filter);
        hci_filter_set_ptype(HCI_EVENT_PKT, &_filter);
        hci_filter_set_event(EVT_CMD_STATUS, &_filter);
        hci_filter_set_event(EVT_CMD_COMPLETE, &_filter);
        hci_filter_set_event(EVT_LE_META_EVENT, &_filter);
        hci_filter_set_event(EVT_LE_CONN_COMPLETE, &_filter);

        // Interesting why this needs to be set.... I hope not!!
        // hci_filter_set_opcode(0, &_filter);

        setsockopt(Handle(), SOL_HCI, HCI_FILTER, &_filter, sizeof(_filter));
    }
}

/* virtual */ uint16_t HCISocket::Deserialize(const uint8_t* dataFrame, const uint16_t availableData)
{
    const hci_event_hdr* hdr = reinterpret_cast<const hci_event_hdr*>(&(dataFrame[1]));
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&(dataFrame[1 + HCI_EVENT_HDR_SIZE]));

    if ( (hdr->evt != EVT_LE_META_EVENT) || (reinterpret_cast<const evt_le_meta_event*>(ptr)->subevent != EVT_LE_ADVERTISING_REPORT) ) {
        Update(*hdr);
    }
    else {
        const le_advertising_info* advertisingInfo = reinterpret_cast<const le_advertising_info*>(&(reinterpret_cast<const evt_le_meta_event*>(ptr)->data[1]));
        uint16_t offset = 0;
        const uint8_t* buffer = advertisingInfo->data;
        const char* name = nullptr;
        uint8_t pos = 0;

        while (((offset + buffer[offset]) <= advertisingInfo->length) && (buffer[offset] != 0)) {

            if (((buffer[offset + 1] == EIR_NAME_SHORT) && (name == nullptr)) || (buffer[offset + 1] == EIR_NAME_COMPLETE)) {
                name = reinterpret_cast<const char*>(&(buffer[offset + 2]));
                pos = buffer[offset] - 1;
            }
            offset += (buffer[offset] + 1);
        }

        if ((name == nullptr) || (pos == 0)) {
            TRACE_L1("Entry[%s] has no name.", Address(advertisingInfo->bdaddr).ToString().c_str());
            Discovered(true, Address(advertisingInfo->bdaddr), _T("[Unknown]"));
        } else {
            Discovered(true, Address(advertisingInfo->bdaddr), string(name, pos));
        }
    }

    return (availableData);
}

void HCISocket::SetOpcode(const uint16_t opcode)
{
    hci_filter_set_opcode(opcode, &_filter);
    setsockopt(Handle(), SOL_HCI, HCI_FILTER, &_filter, sizeof(_filter));
}

/* virtual */ void HCISocket::Update(const hci_event_hdr& eventData)
{
}

/* virtual */ void HCISocket::Discovered(const bool lowEnergy, const Bluetooth::Address& address, const string& name)
{
}

// --------------------------------------------------------------------------------------------------
// ControlSocket !!!
// --------------------------------------------------------------------------------------------------

// ------------------------------------------------------------------------
// Create definitions for the Management commands
// ------------------------------------------------------------------------
// https://github.com/pauloborges/bluez/blob/master/doc/mgmt-api.txt
// ------------------------------------------------------------------------

template <const uint16_t OPCODE, typename OUTBOUND, typename INBOUND>
class ManagementType : public Core::IOutbound, public Core::IInbound {
protected:
    ManagementType(uint8_t buffer[]) : _buffer(buffer) {
    }

public:
    ManagementType() = delete;
    ManagementType(const ManagementType<OPCODE, OUTBOUND, INBOUND>&) = delete;
    ManagementType<OPCODE, OUTBOUND, INBOUND>& operator=(const ManagementType<OPCODE, OUTBOUND, INBOUND>&) = delete;
    ManagementType(const uint16_t size, uint8_t buffer[], const uint16_t adapterIndex)
        : _size(size), _buffer(buffer), _offset(_size), _error(~0), _inboundSize(0)
    {
        _buffer[0] = (OPCODE & 0xFF);
        _buffer[1] = (OPCODE >> 8) & 0xFF;
        _buffer[2] = (adapterIndex & 0xFF);
        _buffer[3] = ((adapterIndex >> 8) & 0xFF);
        _buffer[4] = static_cast<uint8_t>((size - sizeof(mgmt_hdr)) & 0xFF);
        _buffer[5] = static_cast<uint8_t>((size >> 8) & 0xFF);
    }
    virtual ~ManagementType()
    {
    }

public:
    void Clear()
    {
        ::memset(&(_buffer[sizeof(mgmt_hdr)]), 0, _size - sizeof(mgmt_hdr));
    }
    OUTBOUND* operator->()
    {
        return (reinterpret_cast<OUTBOUND*>(&(_buffer[sizeof(mgmt_hdr)])));
    }
    uint16_t Result() const {
        return (_error);
    }
    const INBOUND& Response() 
    {
        return (_inbound);
    }
    uint16_t Loaded () const {
        return (_inboundSize);
    }

private:
    virtual void Reload() const override
    {
        _error = ~0;
        _offset = 0;
        _inboundSize = 0;
    }
    virtual uint16_t Serialize(uint8_t stream[], const uint16_t length) const override
    {
        uint16_t result = std::min(static_cast<uint16_t>(_size - _offset), length);
        if (result > 0) {

            ::memcpy(stream, &(_buffer[_offset]), result);
            _offset += result;

            // printf ("SEND: ");
            // for (uint16_t loop = 0; loop < result; loop++) { printf("%02X:", stream[loop]); } printf("\n");
        }
        return (result);
    }
    virtual uint16_t Deserialize(const uint8_t stream[], const uint16_t length) override
    {
        uint16_t result = 0;
        if (length >= sizeof(mgmt_hdr)) {

            // printf ("RECEIVED: ");
            // for (uint16_t loop = 0; loop < length; loop++) { printf("%02X:", stream[loop]); } printf("\n");

            const mgmt_hdr* hdr = reinterpret_cast<const mgmt_hdr*>(stream);
            uint16_t opCode = htobs(hdr->opcode);
            uint16_t payload = htobs(hdr->len);
            if (opCode == MGMT_EV_CMD_STATUS) {
                uint16_t len = length - sizeof(mgmt_hdr);
                const mgmt_ev_cmd_status* data = reinterpret_cast<const mgmt_ev_cmd_status*>(&(stream[sizeof(mgmt_hdr)])); 
                if (htobs(data->opcode) == OPCODE) { 
                    result = sizeof(mgmt_hdr);
                    if ( (data->status == 0) && (len >= sizeof(mgmt_ev_cmd_status)) ) { 
                        result += sizeof(mgmt_ev_cmd_status); 
                        _error  = Core::ERROR_NONE; 
                    } 
                    else { 
                        result += (len < sizeof(mgmt_ev_cmd_status) ? len : sizeof(mgmt_ev_cmd_status));
                        _error  = Core::ERROR_GENERAL; 
                        TRACE(Trace::Information, (_T(">>EVT_MGMT_STATUS: %04X Error: %d"), data->opcode, data->status )); 
                    } 
                }
            }
            else if (opCode == MGMT_EV_CMD_COMPLETE) { 
                const mgmt_ev_cmd_complete* data = reinterpret_cast<const mgmt_ev_cmd_complete*>(&(stream[sizeof(mgmt_hdr)])); 
                if (htobs(data->opcode) == OPCODE) { 
                    uint16_t len = length - sizeof(mgmt_hdr);
                    result = sizeof(mgmt_hdr);
                    if (len < sizeof(mgmt_ev_cmd_complete)) { 
                        _error = Core::ERROR_GENERAL; 
                        TRACE(Trace::Information, (_T(">>EVT_MGMT_COMPLETED: %04X Error: %d"), data->opcode, _error)); 
                        result += len; 
                    } else { 
                        _inboundSize = std::min(
                            static_cast<uint16_t>(sizeof(INBOUND)), 
                            static_cast<uint16_t>(payload - sizeof (mgmt_ev_cmd_complete)));
                        _inboundSize = std::min(_inboundSize, static_cast<uint16_t>(len - sizeof(mgmt_ev_cmd_complete))); 

                        ::memcpy(reinterpret_cast<uint8_t*>(&_inbound), data->data, _inboundSize); 
                        _error = Core::ERROR_NONE; 
                        result = payload;
                    } 
                } 
            }
        }
        return (result);
    }
    virtual state IsCompleted() const override {
        return (_error != static_cast<uint16_t>(~0) ? state::COMPLETED : state::INPROGRESS);
    }

private:
    uint16_t _size;
    uint8_t* _buffer;
    mutable uint16_t _offset;
    mutable uint16_t _error;
    mutable uint16_t _inboundSize;
    INBOUND _inbound;
};

template <const uint16_t OPCODE, typename OUTBOUND, typename INBOUND>
class ManagementFixedType : public ManagementType<OPCODE,OUTBOUND,INBOUND> {
public:
    ManagementFixedType() = delete;
    ManagementFixedType(const ManagementFixedType<OPCODE, OUTBOUND, INBOUND>&) = delete;
    ManagementFixedType<OPCODE, OUTBOUND, INBOUND>& operator=(const ManagementFixedType<OPCODE, OUTBOUND, INBOUND>&) = delete;

    ManagementFixedType(const uint16_t adapterIndex)
        : ManagementType<OPCODE,OUTBOUND,INBOUND> (sizeof(_buffer), _buffer, adapterIndex) {
    }
    ~ManagementFixedType() {
    }

private:
    uint8_t _buffer[sizeof(mgmt_hdr) + (std::is_same<OUTBOUND, Core::Void>::value ? 0 : sizeof(OUTBOUND))];
};

template <const uint16_t OPCODE, typename OUTBOUND, typename INBOUND, typename LISTTYPE>
class ManagementListType : public ManagementType<OPCODE,OUTBOUND,INBOUND> {
private:
    ManagementListType(const uint16_t size, uint8_t buffer[], const uint16_t adapterIndex)
        : ManagementType<OPCODE,OUTBOUND,INBOUND> (size, buffer, adapterIndex), _buffer(buffer) {
    }
public:
    ManagementListType() = delete;
    ManagementListType(const ManagementListType<OPCODE, OUTBOUND, INBOUND, LISTTYPE>&) = delete;
    ManagementListType<OPCODE, OUTBOUND, INBOUND, LISTTYPE>& operator=(const ManagementListType<OPCODE, OUTBOUND, INBOUND, LISTTYPE>&) = delete;

    ManagementListType(ManagementListType<OPCODE, OUTBOUND, INBOUND, LISTTYPE>&& copy)
        : ManagementType<OPCODE,OUTBOUND,INBOUND> (copy._buffer){
    }

    static ManagementListType<OPCODE, OUTBOUND, INBOUND, LISTTYPE> Instance (const uint16_t adapterIndex, const LISTTYPE& list) {
        uint16_t listLength = (list.Entries() * LISTTYPE::Length());
        uint16_t length = sizeof(mgmt_hdr) + sizeof(OUTBOUND) + listLength;
        uint8_t* buffer = new uint8_t[length];
        ManagementListType<OPCODE, OUTBOUND, INBOUND, LISTTYPE> result(length, buffer, adapterIndex);
        list.Clone(listLength, &(buffer[sizeof(mgmt_hdr) + sizeof(OUTBOUND)]));
        return (result);
    }

    ~ManagementListType() {
        if (_buffer != nullptr) {
            delete [] _buffer;
        }
        _buffer = nullptr;
    }

private:
    uint8_t* _buffer;
};

/* 500 ms to execute a management command. Should be enough for a kernel message exchange. */
static uint32_t MANAGMENT_TIMEOUT = 500; 

static constexpr uint8_t DISABLE_MODE = 0x00;
static constexpr uint8_t ENABLE_MODE  = 0x01;

namespace Management {
    typedef ManagementFixedType<MGMT_OP_SET_POWERED, mgmt_mode, uint32_t> Power;
    typedef ManagementFixedType<MGMT_OP_SET_BONDABLE, mgmt_mode, uint32_t> Bondable;
    typedef ManagementFixedType<MGMT_OP_SET_SSP, mgmt_mode, uint32_t> SimplePairing;
    typedef ManagementFixedType<MGMT_OP_SET_SECURE_CONN, mgmt_mode, uint32_t> SecureConnections;
    typedef ManagementFixedType<MGMT_OP_SET_LE, mgmt_mode, uint32_t> LowEnergy;
    typedef ManagementFixedType<MGMT_OP_SET_ADVERTISING, mgmt_mode, uint32_t> Advertising;
    typedef ManagementFixedType<MGMT_OP_PAIR_DEVICE, mgmt_cp_pair_device, Core::Void> Pair;
    typedef ManagementFixedType<MGMT_OP_UNPAIR_DEVICE, mgmt_cp_unpair_device, Core::Void> Unpair;
    typedef ManagementFixedType<MGMT_OP_READ_INDEX_LIST, Core::Void, uint16_t[33]> Indexes;
    typedef ManagementFixedType<MGMT_OP_SET_LOCAL_NAME, mgmt_cp_set_local_name, mgmt_cp_set_local_name> DeviceName;
    typedef ManagementListType<MGMT_OP_LOAD_LINK_KEYS, mgmt_cp_load_link_keys, uint32_t, ControlSocket::LinkKeyList> LinkKeys;
    typedef ManagementListType<MGMT_OP_LOAD_LONG_TERM_KEYS, mgmt_cp_load_long_term_keys, uint32_t, ControlSocket::LongTermKeyList> LongTermKeys;
    typedef ManagementListType<MGMT_OP_LOAD_IRKS, mgmt_cp_load_irks, uint32_t, ControlSocket::IdentityKeyList> IdentityKeys;

}

/* static */ void ControlSocket::Devices(std::list<uint16_t>& adapters)
{
    Management::Indexes message(HCI_DEV_NONE);
    ControlSocket globalPort;

    if (globalPort.Exchange(MANAGMENT_TIMEOUT, message, message) == Core::ERROR_NONE) {
        if (message.Result() == Core::ERROR_NONE) {
            for (uint16_t index = 0; index < message.Response()[0]; index++) {
                adapters.push_back(message.Response()[index+1]);
            }
        }
    }
}

uint32_t ControlSocket::Name(const string& shortName, const string longName)
{
    Management::DeviceName message(_deviceId);
    std::string shortName2(Core::ToString(shortName.substr(0, sizeof(message->short_name) - 1)));
    std::string longName2(Core::ToString(longName.substr(0, sizeof(message->name) - 1)));
    
    strcpy (reinterpret_cast<char*>(message->short_name), shortName2.c_str());
    strcpy (reinterpret_cast<char*>(message->name), longName2.c_str());

    uint32_t result = Exchange(MANAGMENT_TIMEOUT, message, message);

    return (result != Core::ERROR_NONE ? result : (message.Result() == Core::ERROR_NONE ? result : Core::ERROR_GENERAL));
}

uint32_t ControlSocket::Power(const bool enabled)
{
    Management::Power message(_deviceId);

    message->val = (enabled ? ENABLE_MODE : DISABLE_MODE);

    uint32_t result = Exchange(MANAGMENT_TIMEOUT, message, message);

    return (result != Core::ERROR_NONE ? result : (message.Result() == Core::ERROR_NONE ? result : Core::ERROR_GENERAL));
}

uint32_t ControlSocket::Bondable(const bool enabled)
{
    Management::Bondable message(_deviceId);

    message->val = (enabled ? ENABLE_MODE : DISABLE_MODE);

    uint32_t result = Exchange(MANAGMENT_TIMEOUT, message, message);

    return (result != Core::ERROR_NONE ? result : (message.Result() == Core::ERROR_NONE ? result : Core::ERROR_GENERAL));
}

uint32_t ControlSocket::Advertising(const bool enabled)
{
    Management::Advertising message(_deviceId);

    message->val = (enabled ? ENABLE_MODE : DISABLE_MODE);

    uint32_t result = Exchange(MANAGMENT_TIMEOUT, message, message);

    return (result != Core::ERROR_NONE ? result : (message.Result() == Core::ERROR_NONE ? result : Core::ERROR_GENERAL));
}

uint32_t ControlSocket::SimplePairing(const bool enabled)
{
    Management::SimplePairing message(_deviceId);

    message->val = (enabled ? ENABLE_MODE : DISABLE_MODE);

    uint32_t result = Exchange(MANAGMENT_TIMEOUT, message, message);

    return (result != Core::ERROR_NONE ? result : (message.Result() == Core::ERROR_NONE ? result : Core::ERROR_GENERAL));
}

uint32_t ControlSocket::LowEnergy(const bool enabled)
{
    Management::LowEnergy message(_deviceId);

    message->val = (enabled ? ENABLE_MODE : DISABLE_MODE);

    uint32_t result = Exchange(MANAGMENT_TIMEOUT, message, message);

    return (result != Core::ERROR_NONE ? result : (message.Result() == Core::ERROR_NONE ? result : Core::ERROR_GENERAL));
}

uint32_t ControlSocket::Secure(const bool enabled)
{
    Management::SecureConnections message(_deviceId);

    message->val = (enabled ? ENABLE_MODE : DISABLE_MODE);

    uint32_t result = Exchange(MANAGMENT_TIMEOUT, message, message);

    return (result != Core::ERROR_NONE ? result : (message.Result() == Core::ERROR_NONE ? result : Core::ERROR_GENERAL));
}

uint32_t ControlSocket::LinkKeys(const LinkKeyList& keys, const bool debugKeys) 
{
    uint32_t result = Core::ERROR_UNAVAILABLE;

    if (result == Core::ERROR_UNAVAILABLE) {
        Management::LinkKeys message = Management::LinkKeys::Instance(_deviceId, keys);
        message->key_count = htobs(keys.Entries());
        message->debug_keys = (debugKeys ? ENABLE_MODE : DISABLE_MODE);

        result = Exchange(MANAGMENT_TIMEOUT, message, message);
        result = (result != Core::ERROR_NONE ? result : (message.Result() == Core::ERROR_NONE ? result : Core::ERROR_ASYNC_FAILED));
    }

    return (result);
}

uint32_t ControlSocket::LongTermKeys(const LongTermKeyList& keys) 
{
    uint32_t result = Core::ERROR_UNAVAILABLE;

    if (result == Core::ERROR_UNAVAILABLE) {
        Management::LongTermKeys message = Management::LongTermKeys::Instance(_deviceId, keys);
        message->key_count = htobs(keys.Entries());

        result = Exchange(MANAGMENT_TIMEOUT, message, message);
        result = (result != Core::ERROR_NONE ? result : (message.Result() == Core::ERROR_NONE ? result : Core::ERROR_ASYNC_FAILED));
    }

    return (result);
}

uint32_t ControlSocket::IdentityKeys(const IdentityKeyList& keys) 
{
    uint32_t result = Core::ERROR_UNAVAILABLE;

    if (result == Core::ERROR_UNAVAILABLE) {
        Management::IdentityKeys message = Management::IdentityKeys::Instance(_deviceId, keys);
        message->irk_count = htobs(keys.Entries());

        result = Exchange(MANAGMENT_TIMEOUT, message);
        result = (result != Core::ERROR_NONE ? result : (message.Result() == Core::ERROR_NONE ? result : Core::ERROR_ASYNC_FAILED));
    }

    return (result);
}

uint32_t ControlSocket::Pair(const Address& remote, const uint8_t type, const capabilities cap)
{
    Management::Pair command(_deviceId);

    command.Clear();
    command->addr.bdaddr = *remote.Data();
    command->addr.type = type;
    command->io_cap = cap;

    return (Exchange(MANAGMENT_TIMEOUT, command, command));
}

uint32_t ControlSocket::Unpair(const Address& remote, const uint8_t type)
{
    Management::Unpair command(_deviceId);
    command.Clear();
    command->addr.bdaddr = *remote.Data();
    command->addr.type = type;
    command->disconnect = 1;

    return(Exchange(MANAGMENT_TIMEOUT, command, command));
}

/* virtual */ void ControlSocket::StateChange() 
{
    Core::SynchronousChannelType<Core::SocketPort>::StateChange();
    if (IsOpen() == true) {
        hci_filter_clear(&_filter);
        hci_filter_set_ptype(HCI_EVENT_PKT, &_filter);
        hci_filter_set_event(EVT_CMD_STATUS, &_filter);
        hci_filter_set_event(EVT_CMD_COMPLETE, &_filter);
        hci_filter_set_event(EVT_LE_META_EVENT, &_filter);
        hci_filter_set_event(EVT_LE_CONN_COMPLETE, &_filter);

        // Interesting why this needs to be set.... I hope not!!
        // hci_filter_set_opcode(0, &_filter);

        setsockopt(Handle(), SOL_HCI, HCI_FILTER, &_filter, sizeof(_filter));
    }
}

/* virtual */ uint16_t ControlSocket::Deserialize(const uint8_t* dataFrame, const uint16_t availableData)
{
    if (availableData > sizeof(hci_event_hdr)) {
        const hci_event_hdr* hdr = reinterpret_cast<const hci_event_hdr*>(&(dataFrame[1]));
        Update(*hdr);
    }
    else {
        TRACE(Trace::Information, (_T("]]CONTROLSOCKET: Failure received a too small frame")));
    }

    return (availableData);
}

/* virtual */ void ControlSocket::Update(const hci_event_hdr& eventData)
{
}


} // namespace Bluetooth

} // namespace WPEFramework
