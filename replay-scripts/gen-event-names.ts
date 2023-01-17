// Copyright 2021 Record Replay Inc. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @file In this file, we take all CDT user event names, 
 * parse them and map them against their `gecko` counterpart.
 * It outputs (i) verification results, (ii) comparison results
 * (iii) potential mapping problems, (iv) and C++ code that 
 * allows easily looking up `gecko` event names by their CDT
 * counterpart.
 * 
 * @see https://linear.app/replay/issue/RUN-1061#comment-bde208c4
 */

import fs from 'fs';
import path from 'path';
import _ from 'lodash';
import levenshtein from 'js-levenshtein';
import { EventHandlerType } from "@replayio/protocol";


type EventDefinition = {
  label: string;
  type: EventHandlerType;
  eventTargets?: string[];
};

type EventCategory = {
  category: string;
  events: Array<EventDefinition>;
  eventTargets?: string[];
};



const cdtEventsSrc = `this.createInstrumentationBreakpoints(
        i18nString(UIStrings.animation),
        ['requestAnimationFrame', 'cancelAnimationFrame', 'requestAnimationFrame.callback']);
    this.createInstrumentationBreakpoints(
        i18nString(UIStrings.canvas), ['canvasContextCreated', 'webglErrorFired', 'webglWarningFired']);
    this.createInstrumentationBreakpoints(
        i18nString(UIStrings.geolocation), ['Geolocation.getCurrentPosition', 'Geolocation.watchPosition']);
    this.createInstrumentationBreakpoints(i18nString(UIStrings.notification), ['Notification.requestPermission']);
    this.createInstrumentationBreakpoints(i18nString(UIStrings.parse), ['Element.setInnerHTML', 'Document.write']);
    this.createInstrumentationBreakpoints(i18nString(UIStrings.script), ['scriptFirstStatement', 'scriptBlockedByCSP']);
    this.createInstrumentationBreakpoints(
        i18nString(UIStrings.timer),
        ['setTimeout', 'clearTimeout', 'setInterval', 'clearInterval', 'setTimeout.callback', 'setInterval.callback']);
    this.createInstrumentationBreakpoints(i18nString(UIStrings.window), ['DOMWindow.close']);
    this.createInstrumentationBreakpoints(
        i18nString(UIStrings.webaudio),
        ['audioContextCreated', 'audioContextClosed', 'audioContextResumed', 'audioContextSuspended']);

    this.createEventListenerBreakpoints(
        i18nString(UIStrings.media),
        [
          'play',      'pause',          'playing',    'canplay',    'canplaythrough', 'seeking',
          'seeked',    'timeupdate',     'ended',      'ratechange', 'durationchange', 'volumechange',
          'loadstart', 'progress',       'suspend',    'abort',      'error',          'emptied',
          'stalled',   'loadedmetadata', 'loadeddata', 'waiting',
        ],
        ['audio', 'video']);
    this.createEventListenerBreakpoints(
        i18nString(UIStrings.pictureinpicture), ['enterpictureinpicture', 'leavepictureinpicture'], ['video']);
    this.createEventListenerBreakpoints(i18nString(UIStrings.pictureinpicture), ['resize'], ['PictureInPictureWindow']);
    this.createEventListenerBreakpoints(
        i18nString(UIStrings.clipboard), ['copy', 'cut', 'paste', 'beforecopy', 'beforecut', 'beforepaste'], ['*']);
    this.createEventListenerBreakpoints(
        i18nString(UIStrings.control),
        ['resize', 'scroll', 'zoom', 'focus', 'blur', 'select', 'change', 'submit', 'reset'], ['*']);
    this.createEventListenerBreakpoints(i18nString(UIStrings.device), ['deviceorientation', 'devicemotion'], ['*']);
    this.createEventListenerBreakpoints(
        i18nString(UIStrings.domMutation),
        [
          'DOMActivate',
          'DOMFocusIn',
          'DOMFocusOut',
          'DOMAttrModified',
          'DOMCharacterDataModified',
          'DOMNodeInserted',
          'DOMNodeInsertedIntoDocument',
          'DOMNodeRemoved',
          'DOMNodeRemovedFromDocument',
          'DOMSubtreeModified',
          'DOMContentLoaded',
        ],
        ['*']);
    this.createEventListenerBreakpoints(
        i18nString(UIStrings.dragDrop), ['drag', 'dragstart', 'dragend', 'dragenter', 'dragover', 'dragleave', 'drop'],
        ['*']);

    this.createEventListenerBreakpoints(
        i18nString(UIStrings.keyboard), ['keydown', 'keyup', 'keypress', 'input'], ['*']);
    this.createEventListenerBreakpoints(
        i18nString(UIStrings.load),
        [
          'load',
          'beforeunload',
          'unload',
          'abort',
          'error',
          'hashchange',
          'popstate',
          'navigate',
          'navigatesuccess',
          'navigateerror',
          'currentchange',
          'navigateto',
          'navigatefrom',
          'finish',
          'dispose',
        ],
        ['*']);
    this.createEventListenerBreakpoints(
        i18nString(UIStrings.mouse),
        [
          'auxclick',
          'click',
          'dblclick',
          'mousedown',
          'mouseup',
          'mouseover',
          'mousemove',
          'mouseout',
          'mouseenter',
          'mouseleave',
          'mousewheel',
          'wheel',
          'contextmenu',
        ],
        ['*']);
    this.createEventListenerBreakpoints(
        i18nString(UIStrings.pointer),
        [
          'pointerover',
          'pointerout',
          'pointerenter',
          'pointerleave',
          'pointerdown',
          'pointerup',
          'pointermove',
          'pointercancel',
          'gotpointercapture',
          'lostpointercapture',
          'pointerrawupdate',
        ],
        ['*']);
    this.createEventListenerBreakpoints(
        i18nString(UIStrings.touch), ['touchstart', 'touchmove', 'touchend', 'touchcancel'], ['*']);
    this.createEventListenerBreakpoints(i18nString(UIStrings.worker), ['message', 'messageerror'], ['*']);
    this.createEventListenerBreakpoints(
        i18nString(UIStrings.xhr),
        ['readystatechange', 'load', 'loadstart', 'loadend', 'abort', 'error', 'progress', 'timeout'],
        ['xmlhttprequest', 'xmlhttprequestupload']);`


/** ###########################################################################
 * event normalization
 * ##########################################################################*/

const manualCdtToGeckoLabelOverrides: { [key: string]: string } = {
  // TODO: some events have been removed in gecko. Need to handle that better.

  // 'hashchange': 'hashchange',
  // 'popstate': 'popstate',
  // 'pointerout': 'pointerout',

  // 'beforeinput': 'event.keyboard.beforeinput',
  // these are missing in CDT (but there are many more missing in gecko)
  // '?': 'event.websocket.open',
  // '?': 'event.websocket.message',
  // '?': 'event.websocket.error',
  // '?': 'event.websocket.close',
  // '?': 'event.serviceworker.fetch',
};

const ignoredCdtEventsByCategory: { [key: string]: Set<string> } = {
  load: new Set([
    'beforeunload',
    'unload',
    'navigate',
    'navigatesuccess',
    'navigateerror',
    'currentchange',
    'navigateto',
    'navigatefrom',
    'finish',
    'dispose'
  ]),
  pointer: new Set([
    'pointerrawupdate'
  ])
}

const cdtToGeckoCategoryMap: { [key: string]: string } = {
  Xhr: 'XHR',
  'Drag Drop': 'Drag and Drop',
  'Dom Mutation': 'DOM Mutation'
};

/**
 * Translate category names from CDT to gecko.
 */
function translateCDTToGeckoCategory(category: string) {
  category = _.startCase(category);
  return cdtToGeckoCategoryMap[category] || category;
}

/** ###########################################################################
 * go!
 * ##########################################################################*/

(async () => {
  if (!process.env.REPLAY_DIR) {
    throw new Error(`REPLAY_DIR env variable (parent path of devtools) not found`);
  }
  const devtoolsDir = path.resolve(process.env.REPLAY_DIR as string, 'devtools');
  const eventsSrc = path.resolve(
    devtoolsDir,
    'packages/replay-next/src/constants.ts'
  );
  if (!fs.existsSync(eventsSrc)) {
    throw new Error(`devtools source file for events not found:\n  ${eventsSrc}`);
  }
  const { 
    STANDARD_EVENT_CATEGORIES: geckoEvents
  }: { STANDARD_EVENT_CATEGORIES: EventCategory[] } = await import(eventsSrc);
  console.group(`\n=== Parsing and checking CDT events`);

  const res = cdtEventsSrc.matchAll(
    /i18nString\(UIStrings\.(.+?)\),\s*?(\[[^\]]*\])(?:,\s*?(\[[^\]]*\]))?.*?/gm
  );

  const cdtCategoriesRaw = [...res];


  // check if we (probably/hopefully) parsed CDT src correctly
  const srcCategoryCount = getInputSrcCategoryCount(cdtEventsSrc);
  const cdtCategoryCount = getInputSrcCategoryCount(cdtCategoriesRaw.join('\n'));

  const good = srcCategoryCount > 0 && srcCategoryCount === cdtCategoryCount;
  console.log(`Found ${good ? srcCategoryCount : `${cdtCategoryCount}/${srcCategoryCount}`} CDT source categories (w/ duplicates) ${good ? '✅' : '❌'}`);
  assert(good);

  let cdtEvents = cdtCategoriesRaw.map(cat => {
    const category = cat[1];
    const events = eval(cat[2]);
    const eventTargets = (cat[3] && eval(cat[3]) || ['*']) as string[];
    return {
      // attempt basic normalization
      category: translateCDTToGeckoCategory(category),
      events: events.map((x: string): EventDefinition => ({
          type: x,
          label: x,
          eventTargets
        })
      ) as EventDefinition[],
      eventTargets
    };
  });

  // merge all event sets of the same name
  const cdtEventsByCategory = _.groupBy(cdtEvents, e => e.category);
  cdtEvents = Object.values(cdtEventsByCategory)
    .map(group => {
      if (group.length > 1) {
        console.log(`de-duplicating category: ${group[0].category}`);
        group.forEach((g, i) => g.category = g.category + i)
      }
      return group[0];
    });

  console.log('Done.');
  console.groupEnd();



  /** ###########################################################################
   * compare gecko vs. CDT categories
   * ##########################################################################*/

  console.group(`\n=== Comparing CDT and gecko categories:`);

  console.table({
    CDT: {
      categories: cdtEvents.length,
      events: cdtEvents.flatMap(e => e.events).length
    },
    gecko: {
      categories: geckoEvents.length,
      events: geckoEvents.flatMap(e => e.events).length
    }
  });

  // compare CDT <-> gecko categories
  const categoriesNotInGecko: EventCategory[] = _.differenceBy(cdtEvents, geckoEvents, 'category');
  const categoriesNotInCDT = _.differenceBy(geckoEvents, cdtEvents, 'category');


  console.log(`Found ${categoriesNotInGecko.length} Categories in CDT but not in gecko:\n `,
    categoriesNotInGecko.map(cat => `${cat.category} (${cat.events.length})`).join('\n  '));

  console.log(`\nFound ${categoriesNotInCDT.length} Categories in gecko but not in CDT:\n `,
    categoriesNotInCDT.map(cat => `${cat.category} (${cat.events.length})`).join('\n  '));

  const geckoEventsByCategory = Object.fromEntries(
    geckoEvents.map(category => ([category.category, category]))
  );

  console.groupEnd();


  /** ###########################################################################
   * match CDT against gecko events
   * ##########################################################################*/

  console.group(`\n=== Matching...`);

  let badMatches = new Set();
  let matchesNeedingManualVerification: [string, string][] = [];
  const cdtEventByGeckoType: { [key: string]: EventDefinition } = {};
  for (const cdtCategory of cdtEvents) {
    const geckoCategory = geckoEventsByCategory[cdtCategory.category];
    if (!geckoCategory) {
      // cannot currently support this category
      continue;
    }
    const cdtCategoryName = cdtCategory.category.toLowerCase();
    for (const cdtEvent of cdtCategory.events) {
      const { type: cdtType } = cdtEvent;
      let geckoType: string | undefined;
      if (ignoredCdtEventsByCategory[cdtCategoryName]?.has(cdtType)) {
        continue;
      }
      const geckoLabelOverride = manualCdtToGeckoLabelOverrides[cdtType];
      if (geckoLabelOverride) {
        geckoType = geckoCategory.events.find(e => e.label === geckoLabelOverride)?.type;
        if (!geckoType) {
          // something went wrong
          console.error(`BAD MATCH: manualCdtToGeckoLabelOverrides had unmatched geckoLabel="${geckoLabelOverride}" for cdtType="${cdtType}"`);
          badMatches.add(`<missing geckoType for geckoLabelOverride=${geckoLabelOverride}>`);
          continue;
        }
      }
      let closest: typeof geckoCategory.events[0] | null = null;
      if (!geckoType) {
        // for each CDT type in category (if it has no manual override):
        //     find the best matching gecko type in the same category
        // NOTE: we match against the gecko label, because it is generally a lot closer than the type
        // console.group(`distance matching`);
        closest = _.minBy(geckoCategory.events, geckoEvent => {
          const dist = levenshtein(geckoEvent.label, cdtType);
          // console.log(`${[geckoEvent.label, cdtType]}: ${dist}`);
          return dist;
        })!;
        // console.groupEnd();
        geckoType = closest.type;
      }
      if (cdtEventByGeckoType[geckoType]) {
        if (!manualCdtToGeckoLabelOverrides[cdtEventByGeckoType[geckoType].type]) {
          // bad: the same gecko event matched with more than one CDT event
          //      and the previously existing match is not a manual override.
          badMatches.add(geckoType);
          if (cdtEventByGeckoType[geckoType].label !== closest?.label) {
            console.error(`BAD MATCH in "${cdtCategoryName}": gecko type "${geckoType}" (${closest?.label}) fuzzy-matched CDT types "${cdtEventByGeckoType[geckoType].type}" and "${cdtType}"`);
            delete cdtEventByGeckoType[geckoType];
          }
          else {
            console.error(`BAD MATCH in "${cdtCategoryName}": CDT type "${cdtType}" had invalid match w/ gecko type "${geckoType}" (already matched CDT type "${cdtEventByGeckoType[geckoType].type}")`);
          }
        }
      }
      else {
        cdtEventByGeckoType[geckoType] = cdtEvent;
        if (closest && cdtType !== closest?.label) {
          matchesNeedingManualVerification.push([cdtType, geckoType]);
        }
      }
    }
  }
  console.log('Done.\n');
  console.groupEnd();

  // get all unmatched events
  const matchedGeckoTypes = Object.keys(cdtEventByGeckoType);
  const missingGeckoTypes = _.difference(
    geckoEvents.flatMap(e => e.events.map(ee => ee.type)),
    matchedGeckoTypes
  );

  console.group(`=== Found ${missingGeckoTypes.length} unmatched gecko events:`)
  console.log(`${missingGeckoTypes.join('\n')}\n`);
  console.groupEnd();


  // non-exact matches need manual verification
  matchesNeedingManualVerification = matchesNeedingManualVerification.filter(
    (c, g) => !badMatches.has(g));
  console.group(`=== Found ${matchesNeedingManualVerification.length} matches needing manual verification:`);
  console.log(`${matchesNeedingManualVerification
      .map(([gt, ct]) => `${ct} => ${gt}`)
      .join('\n')
    }`);
  console.groupEnd();

  if (badMatches.size) {
    // we had bad (duplicate) matches
    console.error(`\n\nConvert script FAILED due to ${badMatches.size} bad matches.\n  => Fix up manualCdtToGeckoEventOverrides and try again.`);
    process.exit(-1);
  }


  // finish it!
  //  TODO: maybe use xclip
  const code = genCpp();
  const codeFile = __dirname + "/event-names-code.gen.cc";
  fs.writeFileSync(codeFile, code);
  console.log(`\n=== Generated code has been written to:\n  ${codeFile}\n`);

  /** ###########################################################################
   * generate C++ code for matching
   * ##########################################################################*/

  function genCpp() {
    const eventEntries: string[] = Object.entries(cdtEventByGeckoType)
      .flatMap(([geckoType, cdtEvent]) =>
        cdtEvent.eventTargets!.map(eventTarget => 
        // { String("setTimeout.callback"), {{{String("timer.timeout.fire"), {String("*")}}}} }
        `{ String("${cdtEvent.type}"), {{{String("${geckoType}"), {String("${eventTarget}")}}}} }`
        )
      );

    return `
  // <GENERATED CODE. DO NOT EDIT.>
  // NOTE: This code is generated via \`ts-node scripts/gen-event-names.ts\`
  static const CDTEventEntryMap& getEventEntryMap() {
    DEFINE_STATIC_LOCAL(CDTEventEntryMap, cdtToGeckoMap, 
      ({
        ${eventEntries.join(',\n      ')}
      })
    );
    return cdtToGeckoMap;
  }
  // </GENERATED CODE. DO NOT EDIT.>`;
  }
})();

/** ###########################################################################
 * other utils
 * ##########################################################################*/

function testMeSomeLevenstein() {
  // levenstein tests
  const pairs = [
    ['load', 'load'],
    ['beforeunload', 'load']
  ];
  console.log('\n\nLevenshtein tests:\n' +
    pairs
      .map(([a, b]) => ({
        a, b, n: levenshtein(a, b)
      }))
      .map(({ a, b, n }) => `${a} - ${b} = ${n}`)
      .join('  \n')
  );
}


function getInputSrcCategoryCount(input: string) {
  return [...input.matchAll(/i18nString/gm)].length;
}

function assert(x: any) {
  if (!x) {
    // NOTE: in some terminals, this produces a nice, red error indicator
    process.exit(-1);
  }
}
