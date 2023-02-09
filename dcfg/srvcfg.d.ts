declare module "@devicescript/srvcfg" {
    type integer = number
    type Pin = integer | string
    type HexInt = integer | string

    type ServiceConfig =
        | RotaryEncoderConfig
        | ButtonConfig
        | RelayConfig
        | PowerConfig

    interface DeviceConfig {
        $schema?: string

        /**
         * Name of the device.
         *
         * @example "Acme Corp. SuperIoT v1.3"
         */
        devName: string

        /**
         * Device class code, typically given as a hex number starting with 0x3.
         *
         * @example "0x379ea214"
         */
        devClass: HexInt

        pinJacdac?: Pin

        led?: LedConfig

        /**
         * Services to mount.
         */
        _?: ServiceConfig[]
    }

    interface LedConfig {
        /**
         * If a single mono LED.
         */
        pin?: Pin

        /**
         * RGB LED driven by PWM.
         */
        rgb?: LedChannel[]

        /**
         * Applies to all LED channels/pins.
         */
        activeHigh?: boolean

        /**
         * Defaults to `true` if `pin` is set and `type` is 0.
         * Otherwise `false`.
         */
        isMono?: boolean

        /**
         * 0 - use `pin` or `rgb` as regular pins
         * 1 - use `pin` as WS2812B (supported only on some boards)
         * Other options (in range 100+) are also possible on some boards.
         * 
         * @default 0
         */
        type?: number
    }

    interface LedChannel {
        pin: Pin
        /**
         * Multiplier to compensate for different LED strengths.
         * @minimum 0
         * @maximum 255
         */
        mult?: integer
    }

    interface BaseConfig {
        service: string

        /**
         * Instance/role name to be assigned to service.
         * @example "buttonA"
         */
        name?: string

        /**
         * Service variant (see service definition for possible values).
         */
        variant?: integer
    }

    interface RotaryEncoderConfig extends BaseConfig {
        service: "rotary"
        pin0: Pin
        pin1: Pin
        /**
         * How many clicks for full 360 turn.
         * @default 12
         */
        clicksPerTurn?: integer
        /**
         * Encoder supports "half-clicks".
         */
        dense?: boolean
        /**
         * Invert direction.
         */
        inverted?: boolean
    }

    interface ButtonConfig extends BaseConfig {
        service: "button"
        pin: Pin
        /**
         * This pin is set high when the button is pressed.
         */
        pinBacklight?: Pin
        /**
         * Button is normally active-low and pulled high.
         * This makes it active-high and pulled low.
         */
        activeHigh?: boolean
    }

    interface RelayConfig extends BaseConfig {
        service: "relay"

        /**
         * The driving pin.
         */
        pin: Pin

        /**
         * When set, the relay is considered 'active' when `pin` is low.
         */
        pinActiveLow?: boolean

        /**
         * Active-high pin that indicates the actual state of the relay.
         */
        pinFeedback?: Pin

        /**
         * This pin will be driven when relay is active.
         */
        pinLed?: Pin

        /**
         * Which way to drive the `pinLed`
         */
        ledActiveLow?: boolean

        /**
         * Whether to activate the relay upon boot.
         */
        initalActive?: boolean

        /**
         * Maximum switching current in mA.
         */
        maxCurrent?: integer
    }

    interface PowerConfig extends BaseConfig {
        service: "power"

        /**
         * Always active low.
         */
        pinFault: Pin
        pinEn: Pin
        /**
         * Active-low pin for pulsing battery banks.
         */
        pinPulse?: Pin
        /**
         * Operation mode of pinEn
         * 0 - `pinEn` is active high
         * 1 - `pinEn` is active low
         * 2 - transistor-on-EN setup, where flt and en are connected at limiter
         * 3 - EN should be pulsed at 50Hz (10ms on, 10ms off) to enable the limiter
         */
        mode: integer

        /**
         * How long (in milliseconds) to ignore the `pinFault` after enabling.
         *
         * @default 16
         */
        faultIgnoreMs: integer

        /**
         * Additional power LED to pulse.
         */
        pinLedPulse?: Pin

        /**
         * Pin that is high when we are connected to USB or similar power source.
         */
        pinUsbDetect?: Pin
    }
}