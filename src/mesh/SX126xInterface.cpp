#if RADIOLIB_EXCLUDE_SX126X != 1
#include "SX126xInterface.h"
#include "configuration.h"
#include "error.h"
#include "mesh/NodeDB.h"
#ifdef ARCH_PORTDUINO
#include "PortduinoGlue.h"
#endif

#include "Throttle.h"

// Particular boards might define a different max power based on what their hardware can do, default to max power output if not
// specified (may be dangerous if using external PA and SX126x power config forgotten)
#ifndef SX126X_MAX_POWER
#define SX126X_MAX_POWER 22
#endif

template <typename T>
SX126xInterface<T>::SX126xInterface(LockingArduinoHal *hal, RADIOLIB_PIN_TYPE cs, RADIOLIB_PIN_TYPE irq, RADIOLIB_PIN_TYPE rst,
                                    RADIOLIB_PIN_TYPE busy)
    : RadioLibInterface(hal, cs, irq, rst, busy, &lora), lora(&module)
{
    LOG_DEBUG("SX126xInterface(cs=%d, irq=%d, rst=%d, busy=%d)\n", cs, irq, rst, busy);
}

/// Initialise the Driver transport hardware and software.
/// Make sure the Driver is properly configured before calling init().
/// \return true if initialisation succeeded.
template <typename T> bool SX126xInterface<T>::init()
{

// Typically, the RF switch on SX126x boards is controlled by two signals, which are negations of each other (switched RFIO
// paths). The negation is usually performed in hardware, or (suboptimal design) TXEN and RXEN are the two inputs to this style of
// RF switch. On some boards, there is no hardware negation between CTRL and ¬CTRL, but CTRL is internally connected to DIO2, and
// DIO2's switching is done by the SX126X itself, so the MCU can't control ¬CTRL at exactly the same time. One solution would be
// to set ¬CTRL as SX126X_TXEN or SX126X_RXEN, but they may already be used for another purpose, such as controlling another
// PA/LNA. Keeping ¬CTRL high seems to work, as long CTRL=1, ¬CTRL=1 has the opposite and stable RF path effect as CTRL=0 and
// ¬CTRL=1, this depends on the RF switch, but it seems this usually works. Better hardware design, which is done most the time,
// means this workaround is not necessary.
#ifdef SX126X_ANT_SW // Perhaps add RADIOLIB_NC check, and beforehand define as such if it is undefined, but it is not commonly
                     // used and not part of the 'default' set of pin definitions.
    digitalWrite(SX126X_ANT_SW, HIGH);
    pinMode(SX126X_ANT_SW, OUTPUT);
#endif

#ifdef SX126X_POWER_EN // Perhaps add RADIOLIB_NC check, and beforehand define as such if it is undefined, but it is not commonly
                       // used and not part of the 'default' set of pin definitions.
    digitalWrite(SX126X_POWER_EN, HIGH);
    pinMode(SX126X_POWER_EN, OUTPUT);
#endif

#if ARCH_PORTDUINO
    float tcxoVoltage = 0;
    if (settingsMap[dio3_tcxo_voltage])
        tcxoVoltage = 1.8;
    if (settingsMap[sx126x_ant_sw] != RADIOLIB_NC) {
        digitalWrite(settingsMap[sx126x_ant_sw], HIGH);
        pinMode(settingsMap[sx126x_ant_sw], OUTPUT);
    }
// FIXME: correct logic to default to not using TCXO if no voltage is specified for SX126X_DIO3_TCXO_VOLTAGE
#elif !defined(SX126X_DIO3_TCXO_VOLTAGE)
    float tcxoVoltage =
        0; // "TCXO reference voltage to be set on DIO3. Defaults to 1.6 V, set to 0 to skip." per
           // https://github.com/jgromes/RadioLib/blob/690a050ebb46e6097c5d00c371e961c1caa3b52e/src/modules/SX126x/SX126x.h#L471C26-L471C104
    // (DIO3 is free to be used as an IRQ)
#elif !defined(TCXO_OPTIONAL)
    float tcxoVoltage = SX126X_DIO3_TCXO_VOLTAGE;
    // (DIO3 is not free to be used as an IRQ)
#endif
    if (tcxoVoltage == 0)
        LOG_DEBUG("SX126X_DIO3_TCXO_VOLTAGE not defined, not using DIO3 as TCXO reference voltage\n");
    else
        LOG_DEBUG("SX126X_DIO3_TCXO_VOLTAGE defined, using DIO3 as TCXO reference voltage at %f V\n", tcxoVoltage);

    // FIXME: May want to set depending on a definition, currently all SX126x variant files use the DC-DC regulator option
    bool useRegulatorLDO = false; // Seems to depend on the connection to pin 9/DCC_SW - if an inductor DCDC?

    RadioLibInterface::init();

    if (power > SX126X_MAX_POWER) // Clamp power to maximum defined level
        power = SX126X_MAX_POWER;

    limitPower();

    int res = lora.begin(getFreq(), bw, sf, cr, syncWord, power, preambleLength, tcxoVoltage, useRegulatorLDO);
    // \todo Display actual typename of the adapter, not just `SX126x`
    LOG_INFO("SX126x init result %d\n", res);
    if (res == RADIOLIB_ERR_CHIP_NOT_FOUND)
        return false;

    LOG_INFO("Frequency set to %f\n", getFreq());
    LOG_INFO("Bandwidth set to %f\n", bw);
    LOG_INFO("Power output set to %d\n", power);

    // Overriding current limit
    // (https://github.com/jgromes/RadioLib/blob/690a050ebb46e6097c5d00c371e961c1caa3b52e/src/modules/SX126x/SX126x.cpp#L85) using
    // value in SX126xInterface.h (currently 140 mA) It may or may not be neccessary, depending on how RadioLib functions, from
    // SX1261/2 datasheet: OCP after setting DeviceSel with SetPaConfig(): SX1261 - 60 mA, SX1262 - 140 mA For the SX1268 the IC
    // defaults to 140mA no matter the set power level, but RadioLib set it lower, this would need further checking Default values
    // are: SX1262, SX1268: 0x38 (140 mA), SX1261: 0x18 (60 mA)
    // FIXME: Not ideal to increase SX1261 current limit above 60mA as it can only transmit max 15dBm, should probably only do it
    // if using SX1262 or SX1268
    res = lora.setCurrentLimit(currentLimit);
    LOG_DEBUG("Current limit set to %f\n", currentLimit);
    LOG_DEBUG("Current limit set result %d\n", res);

    if (res == RADIOLIB_ERR_NONE) {
#ifdef SX126X_DIO2_AS_RF_SWITCH
        bool dio2AsRfSwitch = true;
#elif defined(ARCH_PORTDUINO)
        bool dio2AsRfSwitch = false;
        if (settingsMap[dio2_as_rf_switch]) {
            dio2AsRfSwitch = true;
        }
#else
        bool dio2AsRfSwitch = false;
#endif
        res = lora.setDio2AsRfSwitch(dio2AsRfSwitch);
        LOG_DEBUG("Set DIO2 as %sRF switch, result: %d\n", dio2AsRfSwitch ? "" : "not ", res);
    }

    // If a pin isn't defined, we set it to RADIOLIB_NC, it is safe to always do external RF switching with RADIOLIB_NC as it has
    // no effect
#if ARCH_PORTDUINO
    if (res == RADIOLIB_ERR_NONE) {
        LOG_DEBUG("Using MCU pin %i as RXEN and pin %i as TXEN to control RF switching\n", settingsMap[rxen], settingsMap[txen]);
        lora.setRfSwitchPins(settingsMap[rxen], settingsMap[txen]);
    }
#else
#ifndef SX126X_RXEN
#define SX126X_RXEN RADIOLIB_NC
    LOG_DEBUG("SX126X_RXEN not defined, defaulting to RADIOLIB_NC\n");
#endif
#ifndef SX126X_TXEN
#define SX126X_TXEN RADIOLIB_NC
    LOG_DEBUG("SX126X_TXEN not defined, defaulting to RADIOLIB_NC\n");
#endif
    if (res == RADIOLIB_ERR_NONE) {
        LOG_DEBUG("Using MCU pin %i as RXEN and pin %i as TXEN to control RF switching\n", SX126X_RXEN, SX126X_TXEN);
        lora.setRfSwitchPins(SX126X_RXEN, SX126X_TXEN);
    }
#endif
    if (config.lora.sx126x_rx_boosted_gain) {
        uint16_t result = lora.setRxBoostedGainMode(true);
        LOG_INFO("Set RX gain to boosted mode; result: %d\n", result);
    } else {
        uint16_t result = lora.setRxBoostedGainMode(false);
        LOG_INFO("Set RX gain to power saving mode (boosted mode off); result: %d\n", result);
    }

#if 0
    // Read/write a register we are not using (only used for FSK mode) to test SPI comms
    uint8_t crcLSB = 0;
    int err = lora.readRegister(SX126X_REG_CRC_POLYNOMIAL_LSB, &crcLSB, 1);
    if(err != RADIOLIB_ERR_NONE)
        RECORD_CRITICALERROR(CriticalErrorCode_SX1262Failure);

    //if(crcLSB != 0x0f)
    //    RECORD_CRITICALERROR(CriticalErrorCode_SX1262Failure);

    crcLSB = 0x5a;
    err = lora.writeRegister(SX126X_REG_CRC_POLYNOMIAL_LSB, &crcLSB, 1);
    if(err != RADIOLIB_ERR_NONE)
        RECORD_CRITICALERROR(CriticalErrorCode_SX1262Failure);

    err = lora.readRegister(SX126X_REG_CRC_POLYNOMIAL_LSB, &crcLSB, 1);
    if(err != RADIOLIB_ERR_NONE)
        RECORD_CRITICALERROR(CriticalErrorCode_SX1262Failure);

    if(crcLSB != 0x5a)
        RECORD_CRITICALERROR(CriticalErrorCode_SX1262Failure);
    // If we got this far register accesses (and therefore SPI comms) are good
#endif

    if (res == RADIOLIB_ERR_NONE)
        res = lora.setCRC(RADIOLIB_SX126X_LORA_CRC_ON);

    if (res == RADIOLIB_ERR_NONE)
        startReceive(); // start receiving

    return res == RADIOLIB_ERR_NONE;
}

template <typename T> bool SX126xInterface<T>::reconfigure()
{
    RadioLibInterface::reconfigure();

    // set mode to standby
    setStandby();

    // configure publicly accessible settings
    int err = lora.setSpreadingFactor(sf);
    if (err != RADIOLIB_ERR_NONE)
        RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_INVALID_RADIO_SETTING);

    err = lora.setBandwidth(bw);
    if (err != RADIOLIB_ERR_NONE)
        RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_INVALID_RADIO_SETTING);

    err = lora.setCodingRate(cr);
    if (err != RADIOLIB_ERR_NONE)
        RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_INVALID_RADIO_SETTING);

    err = lora.setSyncWord(syncWord);
    if (err != RADIOLIB_ERR_NONE)
        LOG_ERROR("SX126X setSyncWord %s%d\n", radioLibErr, err);
    assert(err == RADIOLIB_ERR_NONE);

    err = lora.setCurrentLimit(currentLimit);
    if (err != RADIOLIB_ERR_NONE)
        LOG_ERROR("SX126X setCurrentLimit %s%d\n", radioLibErr, err);
    assert(err == RADIOLIB_ERR_NONE);

    err = lora.setPreambleLength(preambleLength);
    if (err != RADIOLIB_ERR_NONE)
        LOG_ERROR("SX126X setPreambleLength %s%d\n", radioLibErr, err);
    assert(err == RADIOLIB_ERR_NONE);

    err = lora.setFrequency(getFreq());
    if (err != RADIOLIB_ERR_NONE)
        RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_INVALID_RADIO_SETTING);

    if (power > SX126X_MAX_POWER) // This chip has lower power limits than some
        power = SX126X_MAX_POWER;

    err = lora.setOutputPower(power);
    if (err != RADIOLIB_ERR_NONE)
        LOG_ERROR("SX126X setOutputPower %s%d\n", radioLibErr, err);
    assert(err == RADIOLIB_ERR_NONE);

    startReceive(); // restart receiving

    return RADIOLIB_ERR_NONE;
}

template <typename T> void INTERRUPT_ATTR SX126xInterface<T>::disableInterrupt()
{
    lora.clearDio1Action();
}

template <typename T> void SX126xInterface<T>::setStandby()
{
    checkNotification(); // handle any pending interrupts before we force standby

    int err = lora.standby();

    if (err != RADIOLIB_ERR_NONE)
        LOG_DEBUG("SX126x standby %s%d\n", radioLibErr, err);
    assert(err == RADIOLIB_ERR_NONE);

    isReceiving = false; // If we were receiving, not any more
    activeReceiveStart = 0;
    disableInterrupt();
    completeSending(); // If we were sending, not anymore
    RadioLibInterface::setStandby();
}

/**
 * Add SNR data to received messages
 */
template <typename T> void SX126xInterface<T>::addReceiveMetadata(meshtastic_MeshPacket *mp)
{
    // LOG_DEBUG("PacketStatus %x\n", lora.getPacketStatus());
    mp->rx_snr = lora.getSNR();
    mp->rx_rssi = lround(lora.getRSSI());
}

/** We override to turn on transmitter power as needed.
 */
template <typename T> void SX126xInterface<T>::configHardwareForSend()
{
    RadioLibInterface::configHardwareForSend();
}

// For power draw measurements, helpful to force radio to stay sleeping
// #define SLEEP_ONLY

template <typename T> void SX126xInterface<T>::startReceive()
{
#ifdef SLEEP_ONLY
    sleep();
#else

    setStandby();

    // We use a 16 bit preamble so this should save some power by letting radio sit in standby mostly.
    // Furthermore, we need the PREAMBLE_DETECTED and HEADER_VALID IRQ flag to detect whether we are actively receiving
    int err = lora.startReceiveDutyCycleAuto(preambleLength, 8, RADIOLIB_IRQ_RX_DEFAULT_FLAGS | RADIOLIB_IRQ_PREAMBLE_DETECTED);
    if (err != RADIOLIB_ERR_NONE)
        LOG_ERROR("SX126X startReceiveDutyCycleAuto %s%d\n", radioLibErr, err);
    assert(err == RADIOLIB_ERR_NONE);

    RadioLibInterface::startReceive();

    // Must be done AFTER, starting transmit, because startTransmit clears (possibly stale) interrupt pending register bits
    enableInterrupt(isrRxLevel0);
#endif
}

/** Is the channel currently active? */
template <typename T> bool SX126xInterface<T>::isChannelActive()
{
    // check if we can detect a LoRa preamble on the current channel
    int16_t result;

    setStandby();
    result = lora.scanChannel();
    if (result == RADIOLIB_LORA_DETECTED || result == RADIOLIB_PREAMBLE_DETECTED)
        return true;
    if (result != RADIOLIB_CHANNEL_FREE)
        LOG_ERROR("SX126X scanChannel %s%d\n", radioLibErr, result);
    assert(result != RADIOLIB_ERR_WRONG_MODEM);

    return false;
}

/** Could we send right now (i.e. either not actively receiving or transmitting)? */
template <typename T> bool SX126xInterface<T>::isActivelyReceiving()
{
    // The IRQ status will be cleared when we start our read operation. Check if we've started a header, but haven't yet
    // received and handled the interrupt for reading the packet/handling errors.
    return receiveDetected(lora.getIrqFlags(), RADIOLIB_SX126X_IRQ_HEADER_VALID, RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED);
}

template <typename T> bool SX126xInterface<T>::sleep()
{
    // Not keeping config is busted - next time nrf52 board boots lora sending fails  tcxo related? - see datasheet
    // \todo Display actual typename of the adapter, not just `SX126x`
    LOG_DEBUG("SX126x entering sleep mode\n"); // (FIXME, don't keep config)
    setStandby();                              // Stop any pending operations

    // turn off TCXO if it was powered
    // FIXME - this isn't correct
    // lora.setTCXO(0);

    // put chipset into sleep mode (we've already disabled interrupts by now)
    bool keepConfig = true;
    lora.sleep(keepConfig); // Note: we do not keep the config, full reinit will be needed

#ifdef SX126X_POWER_EN
    digitalWrite(SX126X_POWER_EN, LOW);
#endif

    return true;
}
#endif
