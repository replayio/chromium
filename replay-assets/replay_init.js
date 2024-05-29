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
    // if (!cbs) {
    //   throw new Error(`ReplayJsEvent_emit_failed_unknown_event: ${event}`);
    // }
    log(`DDBG ${!!cbs} ReplayJsEventEmitter.emit("${event}", ${JSON.stringify(args)})`);
    if (cbs) {
      cbs.forEach((cb) => cb(...args));
    }
  },

  emitWithResult(event, ...args) {
    let cbs = this._callbacks[event];
    if (!cbs?.length) {
      throw new Error(`ReplayJsEvent_emitWithResult_failed_unknown_event: ${event}`);
    }
    log(`DDBG ${!!cbs} ReplayJsEventEmitter.emitWithResult("${event}", ${JSON.stringify(args)})`);
    return cbs[0](...args);
  },
};

function initializeReplayJsEvents(ReplayJsEventEmitter) {
  // Set up the event emitter.
  Object.assign(ReplayJsEventEmitter, ReplayJsEventEmitterPrototype);
  ReplayJsEventEmitter._callbacks = {};
}

return initializeReplayJsEvents;
})();
