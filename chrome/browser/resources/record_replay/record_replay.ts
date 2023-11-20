// normal browser code should work, such as:
//
// const opened_window = window.open("");
// 
// function log(msg: string) {
//     console.log(msg);
//     if (opened_window) {
//         opened_window.document.body.textContent += msg;
//     }
// }
// 
// setInterval(() => log("hello again\n"), 1000);

// we should also be able to add mojo bindings or use `chrome.send()`
// and other things mentioned in webui_explainer.md.

console.log("from console.log");
console.debug("from console.debug");
console.warn("from console.warn");
console.error("from console.error");
throw new Error("hello");