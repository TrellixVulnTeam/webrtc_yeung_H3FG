webpackJsonp([1,0],[function(e,t,o){"use strict";function n(e){return e&&e.__esModule?e:{default:e}}var i=o(1),l=n(i),s=o(6),a=n(s),c=o(4),r=n(c);window.VueApp=new l.default({el:"#app",render:function(e){return e(a.default)}});var d=new r.default.Router;["all","active","completed"].forEach(function(e){d.on(e,function(){window.VueApp.filter=e})}),d.configure({notfound:function(){window.location.hash="",window.VueApp.filter="all"}}),d.init()},,function(e,t,o){"use strict";function n(e){return e&&e.__esModule?e:{default:e}}Object.defineProperty(t,"__esModule",{value:!0});var i=o(7),l=n(i);t.default={name:"app",components:{Todos:l.default}}},function(e,t,o){"use strict";function n(e){return e&&e.__esModule?e:{default:e}}Object.defineProperty(t,"__esModule",{value:!0});var i=o(1),l=n(i);t.default={data:function(){return{todos:[],newTodo:"",filter:"all",allDone:!1,editing:null}},filters:{pluralize:function(e){return 1===e?"item":"items"}},directives:{todoFocus:function(e,t){t&&l.default.nextTick(function(t){e.focus()})}},methods:{addTodo:function(){this.todos.push({completed:!1,title:this.newTodo}),this.newTodo=""},deleteTodo:function(e){this.todos=this.todos.filter(function(t){return t!==e})},deleteCompleted:function(){this.todos=this.todos.filter(function(e){return!e.completed})},editTodo:function(e){this.editing=e},doneEdit:function(){this.editing=null}},computed:{remaining:function(){return this.todos.filter(function(e){return!e.completed}).length},completed:function(){return this.todos.filter(function(e){return e.completed}).length},filteredTodos:function(){return"active"===this.filter?this.todos.filter(function(e){return!e.completed}):"completed"===this.filter?this.todos.filter(function(e){return e.completed}):this.todos},allDone:{get:function(){return 0===this.remaining},set:function(e){this.todos.forEach(function(t){t.completed=e})}}}}},,function(e,t){},function(e,t,o){var n,i;n=o(2);var l=o(8);i=n=n||{},"object"!=typeof n.default&&"function"!=typeof n.default||(i=n=n.default),"function"==typeof i&&(i=i.options),i.render=l.render,i.staticRenderFns=l.staticRenderFns,e.exports=n},function(e,t,o){var n,i;o(5),n=o(3);var l=o(9);i=n=n||{},"object"!=typeof n.default&&"function"!=typeof n.default||(i=n=n.default),"function"==typeof i&&(i=i.options),i.render=l.render,i.staticRenderFns=l.staticRenderFns,e.exports=n},function(e,t){e.exports={render:function(){var e=this,t=e.$createElement;return t("div",{attrs:{id:"app"}},[t("Todos")])},staticRenderFns:[]}},function(e,t){e.exports={render:function(){var e=this,t=e.$createElement;return t("section",{staticClass:"todoapp"},[t("header",{staticClass:"header"},[t("h1",["Todos"])," ",t("input",{directives:[{name:"model",rawName:"v-model",value:e.newTodo,expression:"newTodo"}],staticClass:"new-todo",attrs:{type:"text",autofocus:"",autocomplete:"off",placeholder:"What needs to be done?"},domProps:{value:e._s(e.newTodo)},on:{keyup:function(t){13===t.keyCode&&e.addTodo(t)},input:function(t){t.target.composing||(e.newTodo=t.target.value)}}})])," ",t("section",{directives:[{name:"show",rawName:"v-show",value:e.todos.length,expression:"todos.length"}],staticClass:"main"},[t("input",{directives:[{name:"model",rawName:"v-model",value:e.allDone,expression:"allDone"}],staticClass:"toggle-all",attrs:{type:"checkbox"},domProps:{checked:Array.isArray(e.allDone)?e._i(e.allDone,null)>-1:e._q(e.allDone,!0)},on:{change:function(t){var o=e.allDone,n=t.target,i=!!n.checked;if(Array.isArray(o)){var l=null,s=e._i(o,l);i?s<0&&(e.allDone=o.concat(l)):s>-1&&(e.allDone=o.slice(0,s).concat(o.slice(s+1)))}else e.allDone=i}}})," ",t("ul",{staticClass:"todo-list"},[e._l(e.filteredTodos,function(o){return t("li",{staticClass:"todo",class:{completed:o.completed,editing:o===e.editing}},[t("div",{staticClass:"view"},[t("input",{directives:[{name:"model",rawName:"v-model",value:o.completed,expression:"todo.completed"}],staticClass:"toggle",attrs:{type:"checkbox"},domProps:{checked:Array.isArray(o.completed)?e._i(o.completed,null)>-1:e._q(o.completed,!0)},on:{change:function(t){var n=o.completed,i=t.target,l=!!i.checked;if(Array.isArray(n)){var s=null,a=e._i(n,s);l?a<0&&(o.completed=n.concat(s)):a>-1&&(o.completed=n.slice(0,a).concat(n.slice(a+1)))}else o.completed=l}}})," ",t("label",{on:{dblclick:function(t){e.editTodo(o)}}},[e._s(o.title)])," ",t("button",{staticClass:"destroy",on:{click:function(t){t.preventDefault(),e.deleteTodo(o)}}})])," ",t("input",{directives:[{name:"model",rawName:"v-model",value:o.title,expression:"todo.title"},{name:"todoFocus",rawName:"v-todoFocus",value:o===e.editing,expression:"todo === editing"}],staticClass:"edit",attrs:{type:"text"},domProps:{value:e._s(o.title)},on:{keyup:function(t){13===t.keyCode&&e.doneEdit(t)},blur:e.doneEdit,input:function(e){e.target.composing||(o.title=e.target.value)}}})])})])," "," ",t("footer",{directives:[{name:"show",rawName:"v-show",value:e.todos.length>0,expression:"todos.length > 0"}],staticClass:"footer"},[t("span",{staticClass:"todo-count"},[t("strong",[e._s(e.remaining)])," "+e._s(e._f("pluralize")(e.remaining))+" left\n        "])," ",t("ul",{staticClass:"filters"},[t("li",[t("a",{class:{selected:"all"==e.filter},attrs:{href:"#/all"},on:{click:function(t){e.filter="all"}}},["All"])])," ",t("li",[t("a",{class:{selected:"active"==e.filter},attrs:{href:"#/active"},on:{click:function(t){e.filter="active"}}},["Active"])])," ",t("li",[t("a",{class:{selected:"completed"==e.filter},attrs:{href:"#/completed"},on:{click:function(t){e.filter="completed"}}},["Completed"])])])," ",t("button",{directives:[{name:"show",rawName:"v-show",value:e.completed,expression:"completed"}],staticClass:"clear-completed",on:{click:function(t){t.preventDefault(),e.deleteCompleted(t)}}},["Clear Completed"])])])])},staticRenderFns:[]}}]);
//# sourceMappingURL=app.b0e835874bc8949670d1.js.map