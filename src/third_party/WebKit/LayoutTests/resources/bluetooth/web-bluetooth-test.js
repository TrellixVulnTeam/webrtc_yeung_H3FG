(() => {
  let mojo_;

  async function loadFakeBluetoothInterfaces() {
    if(typeof mojo_ !== 'undefined') {
      return mojo_;
    }

    if (typeof loadMojoModules === 'undefined') {
      throw 'Mojo is required for this API.'
    }

    mojo_ = await loadMojoModules('fakeBluetooth', [
      'mojo/public/js/bindings',
      'device/bluetooth/public/interfaces/test/fake_bluetooth.mojom'
    ]);

    [mojo_.bindings, {
      CentralState: mojo_.CentralState,
      FakeBluetooth: mojo_.FakeBluetooth,
      FakeBluetoothPtr: mojo_.FakeBluetoothPtr,
      FakeCentral: mojo_.FakeCentral,
      FakeCentralPtr: mojo_.FakeCentralPtr,
      CharacteristicProperties: mojo_.CharacteristicProperties,
    }] = mojo_.modules;

    return mojo_;
  }

  function toMojoCentralState(state) {
    switch (state) {
      case 'absent':
        return mojo_.CentralState.ABSENT;
      case 'powered-off':
        return mojo_.CentralState.POWERED_OFF;
      case 'powered-on':
        return mojo_.CentralState.POWERED_ON;
      default:
        throw `Unsupported value ${state} for state.`;
    }
  }

  // Mapping of the property names of
  // BluetoothCharacteristicProperties defined in
  // https://webbluetoothcg.github.io/web-bluetooth/#characteristicproperties
  // to property names of the CharacteristicProperties mojo struct.
  const CHARACTERISTIC_PROPERTIES_WEB_TO_MOJO = {
    broadcast: 'broadcast',
    read: 'read',
    write_without_response: 'write_without_response',
    write: 'write',
    notify: 'notify',
    indicate: 'indicate',
    authenticatedSignedWrites: 'authenticated_signed_writes',
    extended_properties: 'extended_properties',
  };

  function ArrayToMojoCharacteristicProperties(arr) {
    let struct = new mojo_.CharacteristicProperties();

    arr.forEach(val => {
      let mojo_property =
        CHARACTERISTIC_PROPERTIES_WEB_TO_MOJO[val];

      if (struct.hasOwnProperty(mojo_property))
        struct[mojo_property] = true;
      else
        throw `Invalid member '${val}' for CharacteristicProperties`;
    });

    return struct;
  }

  class FakeBluetooth {
    constructor() {
      this.fake_bluetooth_ptr_ = undefined;
    }

    // Set it to indicate whether the platform supports BLE. For example,
    // Windows 7 is a platform that doesn't support Low Energy. On the other
    // hand Windows 10 is a platform that does support LE, even if there is no
    // Bluetooth radio present.
    async setLESupported(supported) {
      // Call setBluetoothFakeAdapter() to clean up any fake adapters left over
      // by legacy tests.
      // Legacy tests that use setBluetoothFakeAdapter() sometimes fail to clean
      // their fake adapter. This is not a problem for these tests because the
      // next setBluetoothFakeAdapter() will clean it up anyway but it is a
      // problem for the new tests that do not use setBluetoothFakeAdapter().
      // TODO(crbug.com/569709): Remove once setBluetoothFakeAdapter is no
      // longer used.
      await setBluetoothFakeAdapter('');
      await this.initFakeBluetoothInterfacePtr_();

      if (typeof supported !== 'boolean') throw 'Type Not Supported';
      await this.fake_bluetooth_ptr_.setLESupported(supported);
    }

    // Returns a promise that resolves with a FakeCentral that clients can use
    // to simulate events that a device in the Central/Observer role would
    // receive as well as monitor the operations performed by the device in the
    // Central/Observer role.
    // Calls sets LE as supported.
    //
    // A "Central" object would allow its clients to receive advertising events
    // and initiate connections to peripherals i.e. operations of two roles
    // defined by the Bluetooth Spec: Observer and Central.
    // See Bluetooth 4.2 Vol 3 Part C 2.2.2 "Roles when Operating over an
    // LE Physical Transport".
    async simulateCentral({state}) {
      // Call setBluetoothFakeAdapter() to clean up any fake adapters left over
      // by legacy tests.
      // Legacy tests that use setBluetoothFakeAdapter() sometimes fail to clean
      // their fake adapter. This is not a problem for these tests because the
      // next setBluetoothFakeAdapter() will clean it up anyway but it is a
      // problem for the new tests that do not use setBluetoothFakeAdapter().
      // TODO(crbug.com/569709): Remove once setBluetoothFakeAdapter is no
      // longer used.
      await setBluetoothFakeAdapter('');
      await this.initFakeBluetoothInterfacePtr_();

      await this.setLESupported(true);

      let {fake_central:fake_central_ptr} =
        await this.fake_bluetooth_ptr_.simulateCentral(
          toMojoCentralState(state));
      return new FakeCentral(fake_central_ptr);
    }

    async initFakeBluetoothInterfacePtr_() {
      if (typeof this.fake_bluetooth_ptr_ !== 'undefined') {
        return this.fake_bluetooth_ptr_;
      }

      let mojo = await loadFakeBluetoothInterfaces();

      this.fake_bluetooth_ptr_ = new mojo.FakeBluetoothPtr(
        mojo.interfaces.getInterface(mojo.FakeBluetooth.name));
    }
  }

  // FakeCentral allows clients to simulate events that a device in the
  // Central/Observer role would receive as well as monitor the operations
  // performed by the device in the Central/Observer role.
  class FakeCentral {
    constructor(fake_central_ptr) {
      this.fake_central_ptr_ = fake_central_ptr;
      this.peripherals_ = new Map();
    }

    // Simulates a peripheral with |address|, |name| and |known_service_uuids|
    // that has already been connected to the system. If the peripheral existed
    // already it updates its name and known UUIDs. |known_service_uuids| should
    // be an array of BluetoothServiceUUIDs
    // https://webbluetoothcg.github.io/web-bluetooth/#typedefdef-bluetoothserviceuuid
    //
    // Platforms offer methods to retrieve devices that have already been
    // connected to the system or weren't connected through the UA e.g. a user
    // connected a peripheral through the system's settings. This method is
    // intended to simulate peripherals that those methods would return.
    async simulatePreconnectedPeripheral({
      address, name, knownServiceUUIDs = []}) {

      // Canonicalize and convert to mojo UUIDs.
      knownServiceUUIDs.forEach((val, i, arr) => {
        knownServiceUUIDs[i] = {uuid: BluetoothUUID.getService(val)};
      });

      await this.fake_central_ptr_.simulatePreconnectedPeripheral(
        address, name, knownServiceUUIDs);

      let peripheral = this.peripherals_.get(address);
      if (peripheral === undefined) {
        peripheral = new FakePeripheral(address, this.fake_central_ptr_);
        this.peripherals_.set(address, peripheral);
      }

      return peripheral;
    }
  }

  class FakePeripheral {
    constructor(address, fake_central_ptr) {
      this.address = address;
      this.fake_central_ptr_ = fake_central_ptr;
    }

    // Sets the next GATT Connection request response to |code|. |code| could be
    // an HCI Error Code from BT 4.2 Vol 2 Part D 1.3 List Of Error Codes or a
    // number outside that range returned by specific platforms e.g. Android
    // returns 0x101 to signal a GATT failure
    // https://developer.android.com/reference/android/bluetooth/BluetoothGatt.html#GATT_FAILURE
    async setNextGATTConnectionResponse({code}) {
      let {success} =
        await this.fake_central_ptr_.setNextGATTConnectionResponse(
          this.address, code);

      if (success !== true) throw 'setNextGATTConnectionResponse failed.';
    }

    // Adds a fake GATT Service with |uuid| to be discovered when discovering
    // the peripheral's GATT Attributes. Returns a FakeRemoteGATTService
    // corresponding to this service. |uuid| should be a BluetoothServiceUUIDs
    // https://webbluetoothcg.github.io/web-bluetooth/#typedefdef-bluetoothserviceuuid
    async addFakeService({uuid}) {
      let {service_id} = await this.fake_central_ptr_.addFakeService(
        this.address, {uuid: BluetoothUUID.getService(uuid)});

      if (service_id === null) throw 'addFakeService failed';

      return new FakeRemoteGATTService(
        service_id, this.address, this.fake_central_ptr_);
    }

    // Sets the next GATT Discovery request response for peripheral with
    // |address| to |code|. |code| could be an HCI Error Code from
    // BT 4.2 Vol 2 Part D 1.3 List Of Error Codes or a number outside that
    // range returned by specific platforms e.g. Android returns 0x101 to signal
    // a GATT failure
    // https://developer.android.com/reference/android/bluetooth/BluetoothGatt.html#GATT_FAILURE
    //
    // The following procedures defined at BT 4.2 Vol 3 Part G Section 4.
    // "GATT Feature Requirements" are used to discover attributes of the
    // GATT Server:
    //  - Primary Service Discovery
    //  - Relationship Discovery
    //  - Characteristic Discovery
    //  - Characteristic Descriptor Discovery
    // This method aims to simulate the response once all of these procedures
    // have completed or if there was an error during any of them.
    async setNextGATTDiscoveryResponse({code}) {
      let {success} =
        await this.fake_central_ptr_.setNextGATTDiscoveryResponse(
          this.address, code);

      if (success !== true) throw 'setNextGATTDiscoveryResponse failed.';
    }

    // Simulates an Indication from the peripheral's GATT `Service Changed`
    // Characteristic from BT 4.2 Vol 3 Part G 7.1. This Indication is signaled
    // when services, characteristics, or descriptors are changed, added, or
    // removed.
    //
    // The value for `Service Changed` is a range of attribute handles that have
    // changed. However, this testing specification works at an abstracted
    // level and does not expose setting attribute handles when adding
    // attributes. Consequently, this simulate method should include the full
    // range of all the peripheral's attribute handle values.
    async simulateGATTServicesChanged() {
      let {success} =
        await this.fake_central_ptr_.simulateGATTServicesChanged(this.address);

      if (success !== true) throw 'simulateGATTServicesChanged failed.';
    }
  }

  class FakeRemoteGATTService {
    constructor(service_id, peripheral_address, fake_central_ptr) {
      this.service_id_ = service_id;
      this.peripheral_address_ = peripheral_address;
      this.fake_central_ptr_ = fake_central_ptr;
    }

    // Adds a fake GATT Characteristic with |uuid| and |properties|
    // to this fake service. The characteristic will be found when discovering
    // the peripheral's GATT Attributes. Returns a FakeRemoteGATTCharacteristic
    // corresponding to the added characteristic.
    async addFakeCharacteristic({uuid, properties}) {
      let {characteristic_id} = await this.fake_central_ptr_.addFakeCharacteristic(
        {uuid: BluetoothUUID.getCharacteristic(uuid)},
        ArrayToMojoCharacteristicProperties(properties),
        this.service_id_,
        this.peripheral_address_
      );

      if (characteristic_id === null) throw 'addFakeCharacteristic failed';

      return new FakeRemoteGATTCharacteristic(
        characteristic_id, this.service_id_,
        this.peripheral_address_, this.fake_central_ptr_);
    }
  }

  class FakeRemoteGATTCharacteristic {
    constructor(characteristic_id, service_id, peripheral_address, fake_central_ptr) {
      this.ids_ = [characteristic_id, service_id, peripheral_address];
      this.descriptors_ = [];
      this.fake_central_ptr_ = fake_central_ptr;
    }

    // Adds a fake GATT Descriptor with |uuid| to be discovered when
    // discovering the peripheral's GATT Attributes. Returns a
    // FakeRemoteGATTDescriptor corresponding to this descriptor. |uuid| should
    // be a BluetoothDescriptorUUID
    // https://webbluetoothcg.github.io/web-bluetooth/#typedefdef-bluetoothdescriptoruuid
    async addFakeDescriptor({uuid}) {
      let {descriptor_id} = await this.fake_central_ptr_.addFakeDescriptor(
        {uuid: BluetoothUUID.getDescriptor(uuid)}, ...this.ids_);

      if (descriptor_id === null) throw 'addFakeDescriptor failed';

      let fake_descriptor = new FakeRemoteGATTDescriptor(
        descriptor_id, ...this.ids_, this.fake_central_ptr_);
      this.descriptors_.push(fake_descriptor);

      return fake_descriptor;
    }

    // Sets the next read response for characteristic to |code| and |value|.
    // |code| could be a GATT Error Response from
    // BT 4.2 Vol 3 Part F 3.4.1.1 Error Response or a number outside that range
    // returned by specific platforms e.g. Android returns 0x101 to signal a GATT
    // failure.
    // https://developer.android.com/reference/android/bluetooth/BluetoothGatt.html#GATT_FAILURE
    async setNextReadResponse(gatt_code, value=null) {
      if (gatt_code === 0 && value === null) {
        throw '|value| can\'t be null if read should success.';
      }
      if (gatt_code !== 0 && value !== null) {
        throw '|value| must be null if read should fail.';
      }

      let {success} =
        await this.fake_central_ptr_.setNextReadCharacteristicResponse(
          gatt_code, value, ...this.ids_);

      if (!success) throw 'setNextReadCharacteristicResponse failed';
    }

    // Sets the next write response for this characteristic to |code|. If
    // writing to a characteristic that only supports 'write_without_response'
    // the set response will be ignored.
    // |code| could be a GATT Error Response from
    // BT 4.2 Vol 3 Part F 3.4.1.1 Error Response or a number outside that range
    // returned by specific platforms e.g. Android returns 0x101 to signal a GATT
    // failure.
    async setNextWriteResponse(gatt_code) {
      let {success} =
        await this.fake_central_ptr_.setNextWriteCharacteristicResponse(
          gatt_code, ...this.ids_);

      if (!success) throw 'setNextWriteResponse failed';
    }

    // Gets the last successfully written value to the characteristic.
    // Returns null if no value has yet been written to the characteristic.
    async getLastWrittenValue() {
      let {success, value} =
        await this.fake_central_ptr_.getLastWrittenValue(...this.ids_);

      if (!success) throw 'getLastWrittenValue failed';

      return value;
    }

    // Removes the fake GATT Characteristic from its fake service.
    async remove() {
      let {success} =
          await this.fake_central_ptr_.removeFakeCharacteristic(...this.ids_);

      if (!success) throw 'remove failed';
    }
  }

  class FakeRemoteGATTDescriptor {
    constructor(descriptor_id,
                characteristic_id,
                service_id,
                peripheral_address,
                fake_central_ptr) {
      this.ids_ = [
        descriptor_id, characteristic_id, service_id, peripheral_address];
      this.fake_central_ptr_ = fake_central_ptr;
    }

    // Sets the next read response for descriptor to |code| and |value|.
    // |code| could be a GATT Error Response from
    // BT 4.2 Vol 3 Part F 3.4.1.1 Error Response or a number outside that range
    // returned by specific platforms e.g. Android returns 0x101 to signal a GATT
    // failure.
    // https://developer.android.com/reference/android/bluetooth/BluetoothGatt.html#GATT_FAILURE
    async setNextReadResponse(gatt_code, value=null) {
      if (gatt_code === 0 && value === null) {
        throw '|value| can\'t be null if read should success.';
      }
      if (gatt_code !== 0 && value !== null) {
        throw '|value| must be null if read should fail.';
      }

      let {success} =
        await this.fake_central_ptr_.setNextReadDescriptorResponse(
          gatt_code, value, ...this.ids_);

      if (!success) throw 'setNextReadDescriptorResponse failed';
    }
  }

  navigator.bluetooth.test = new FakeBluetooth();
})();
