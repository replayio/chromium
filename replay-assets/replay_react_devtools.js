// Script that injects React DevTools "stub" functions to capture
// marker annotations while recording, for use in later processing
(() => {

const stubFiberRoots = {};
const unmountedFibersByRenderer = {};
const unmountedFiberAlternatesByRenderer = {};

const stubHook = {
    isStub: true,
    supportsFiber: true,
    inject,
    onCommitFiberUnmount,
    onCommitFiberRoot,
    onPostCommitFiberRoot,
    renderers: new Map(),
};


function getFiberRootsSetForRenderer(rendererID) {
    if (!stubFiberRoots[rendererID]) {
    stubFiberRoots[rendererID] = new Set();
    }

    return stubFiberRoots[rendererID];
}

function getUnmountedFibersSetForRenderer(rendererID) {
    if (!unmountedFibersByRenderer[rendererID]) {
    unmountedFibersByRenderer[rendererID] = new Set();
    }

    return unmountedFibersByRenderer[rendererID];
}

function getUnmountedFiberAlternatesForRenderer(rendererID) {
    if (!unmountedFiberAlternatesByRenderer[rendererID]) {
    unmountedFiberAlternatesByRenderer[rendererID] = new Map();
    }

    return unmountedFiberAlternatesByRenderer[rendererID];
}

window.__REACT_DEVTOOLS_SAVED_RENDERERS__ = [];
window.__REACT_DEVTOOLS_STUB_FIBER_ROOTS = stubFiberRoots;

Object.defineProperty(window, "__REACT_DEVTOOLS_GLOBAL_HOOK__", {
    configurable: true,
    enumerable: false,
    get() {
    return stubHook;
    }
});

let uidCounter = 0;

function inject(renderer) {
    // Declare these enum strings in scope for later routine use
    const annotationType = "inject";

    const id = ++uidCounter;
    window.__RECORD_REPLAY_ANNOTATION_HOOK__("react-devtools-hook:v1:" + annotationType, "");
    window.__REACT_DEVTOOLS_SAVED_RENDERERS__.push(renderer);
    return id;
}

function onCommitFiberUnmount(rendererID, fiber) {
    const annotationType = "commit-fiber-unmount"

    // Unmounts are always one fiber at a time during the commit phase.
    // Stash the unmounted fibers here, so we can map them to persistent
    // object IDs inside of `onCommitFiberRoot` processing in the routine.
    const unmountedFibersSet = getUnmountedFibersSetForRenderer(rendererID);
    unmountedFibersSet.add(fiber);

    let unmountedFiberAlternates;
    if (fiber.alternate) {
    unmountedFiberAlternates = getUnmountedFiberAlternatesForRenderer(rendererID);
    unmountedFiberAlternates.set(fiber, fiber.alternate);
    }

    window.__RECORD_REPLAY_ANNOTATION_HOOK__("react-devtools-hook:v1:" + annotationType, "");
}

// eslint-disable-next-line no-unused-vars
function onCommitFiberRoot(rendererID, root, priorityLevel) {
    // The "commit" handler should be the only one the routine needs to do the work as of 2023-05-01.
    // We capture unmounted fibers in the unmount handler above, and the routine
    // will process them when we evaluate at the commit annotation point.
    // The others mostly exist for hypothetical completeness.
    const annotationType = "commit-fiber-root";

    const mountedRoots = getFiberRootsSetForRenderer(rendererID);
    const current = root.current;
    const isKnownRoot = mountedRoots.has(root);
    // Keep track of mounted roots so we can hydrate when DevTools connect.
    const isUnmounting = current.memoizedState == null || current.memoizedState.element == null;

    if (!isKnownRoot && !isUnmounting) {
    mountedRoots.add(root);
    } else if (isKnownRoot && isUnmounting) {
    mountedRoots.delete(root);
    }

    // Get these so it's in scope in the routine eval, and we can clear it after the annotation
    const unmountedFibersSet = getUnmountedFibersSetForRenderer(rendererID);
    const unmountedFiberAlternates = getUnmountedFiberAlternatesForRenderer(rendererID);

    window.__RECORD_REPLAY_ANNOTATION_HOOK__("react-devtools-hook:v1:" + annotationType, "");

    for (const fiber of unmountedFibersSet) {
    unmountedFiberAlternates.delete(fiber);
    }
    unmountedFibersSet.clear();
}

// eslint-disable-next-line no-unused-vars
function onPostCommitFiberRoot(rendererID, root) {
    const annotationType = "post-commit-fiber-root";
    window.__RECORD_REPLAY_ANNOTATION_HOOK__("react-devtools-hook:v1:" + annotationType, "");
}

})();
