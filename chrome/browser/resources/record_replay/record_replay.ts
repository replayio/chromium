type RecordReplayArguments = {
    log: (message: string) => void;
    // ...
};

type RecordReplayWindow = Window & typeof globalThis & {
    __RECORD_REPLAY_ARGUMENTS__: RecordReplayArguments;
};

(window as RecordReplayWindow).__RECORD_REPLAY_ARGUMENTS__.log("HELLOOOOOOOOO FROM THE JS FILE");

window.open("https://www.google.com/");