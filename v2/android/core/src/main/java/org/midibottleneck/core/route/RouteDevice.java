package org.midibottleneck.core.route;

/**
 * One selectable MIDI output as a route lists it: one entry per output port (a device with four ports gives four
 * entries). {@code index} is the position in the list and the argument of {@link MidiRoute#open(int)}.
 * Plain data, no Android classes.
 */
public final class RouteDevice {
    public final int index;
    /** Android's id of the device (MidiDeviceInfo.getId()); stable while the device stays connected. */
    public final int deviceId;
    public final String name;
    public final String manufacturer;
    public final String product;
    /** "USB", "Virtual", "Bluetooth" or "Other (n)". */
    public final String typeName;
    /** Number of the port on the device (0 based) that this entry sends to. */
    public final int portNumber;
    public final String portName;
    /** Number of output ports (ports the player can send to) the device has. */
    public final int portCount;

    public RouteDevice(int index, int deviceId, String name, String manufacturer, String product, String typeName,
                       int portNumber, String portName, int portCount) {
        this.index = index;
        this.deviceId = deviceId;
        this.name = name;
        this.manufacturer = manufacturer;
        this.product = product;
        this.typeName = typeName;
        this.portNumber = portNumber;
        this.portName = portName;
        this.portCount = portCount;
    }

    /** Name for a list: the device name, plus the port name when the device has more than one port. */
    public String displayName() {
        if (portCount > 1) {
            String port = portName != null && !portName.isEmpty() ? portName : "port " + (portNumber + 1);
            return name + " - " + port;
        }
        return name;
    }

    /** One line with the details: name, manufacturer, product, type, id, port. */
    public String details() {
        StringBuilder text = new StringBuilder(displayName());
        text.append(" | ").append(typeName);
        if (manufacturer != null && !manufacturer.isEmpty()) text.append(" | manufacturer ").append(manufacturer);
        if (product != null && !product.isEmpty()) text.append(" | product ").append(product);
        text.append(" | id ").append(deviceId);
        text.append(" | output port ").append(portNumber + 1).append(" of ").append(portCount);
        return text.toString();
    }

    @Override
    public String toString() { return displayName(); }
}
