(() => {
/** ###########################################################################
 * ReplayJs: Internal event handling.
 * ##########################################################################*/

const ReplayJsEventEmitterPrototype = {
  on(event, cb) {
    this._callbacks[event] ||= [];
    this._callbacks[event].push(cb);
  },

  emit(event, ...args) {
    let cbs = this._callbacks[event];
    if (cbs) {
      cbs.forEach((cb) => cb(...args));
    }
  },

  emitWithResult(event, ...args) {
    let cbs = this._callbacks[event];
    if (!cbs?.length) {
      // If the caller expects a return value, there must be at least one callback registered.
      throw new Error(`ReplayJsEvent_emitWithResult_failed_unknown_event: ${event}`);
    }
    const rv = cbs[0](...args);
    return rv;
  },
};

function initializeReplayJsEvents(ReplayJsEventEmitter) {
  // Set up the event emitter.
  Object.assign(ReplayJsEventEmitter, ReplayJsEventEmitterPrototype);
  ReplayJsEventEmitter._callbacks = {};
}

return initializeReplayJsEvents;
})();
