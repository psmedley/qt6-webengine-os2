// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import './strings.m.js';
import './item.js';
import './toolbar.js';
import 'chrome://resources/cr_components/managed_footnote/managed_footnote.js';
import 'chrome://resources/cr_elements/cr_button/cr_button.m.js';
import 'chrome://resources/cr_elements/cr_page_host_style_css.js';
import 'chrome://resources/cr_elements/hidden_style_css.m.js';
import 'chrome://resources/cr_elements/shared_style_css.m.js';
import 'chrome://resources/cr_elements/shared_vars_css.m.js';
import 'chrome://resources/polymer/v3_0/iron-list/iron-list.js';

import {getToastManager} from 'chrome://resources/cr_elements/cr_toast/cr_toast_manager.js';
import {FindShortcutBehavior} from 'chrome://resources/cr_elements/find_shortcut_behavior.js';
import {assert} from 'chrome://resources/js/assert.m.js';
import {EventTracker} from 'chrome://resources/js/event_tracker.m.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.m.js';
import {PromiseResolver} from 'chrome://resources/js/promise_resolver.m.js';
import {IronListElement} from 'chrome://resources/polymer/v3_0/iron-list/iron-list.js';
import {html, mixinBehaviors, PolymerElement} from 'chrome://resources/polymer/v3_0/polymer/polymer_bundled.min.js';

import {BrowserProxy} from './browser_proxy.js';
import {States} from './constants.js';
import {MojomData} from './data.js';
import {PageCallbackRouter, PageHandlerInterface} from './downloads.mojom-webui.js';
import {SearchService} from './search_service.js';
import {DownloadsToolbarElement} from './toolbar.js';

declare global {
  interface Window {
    // https://github.com/microsoft/TypeScript/issues/40807
    requestIdleCallback(callback: () => void): void;
  }
}

export interface DownloadsManagerElement {
  $: {
    'toolbar': DownloadsToolbarElement,
    'downloadsList': IronListElement,
  };
}

const DownloadsManagerElementBase =
    mixinBehaviors([FindShortcutBehavior], PolymerElement) as
    {new (): PolymerElement & FindShortcutBehavior};

export class DownloadsManagerElement extends DownloadsManagerElementBase {
  static get is() {
    return 'downloads-manager';
  }

  static get properties() {
    return {
      hasDownloads_: {
        observer: 'hasDownloadsChanged_',
        type: Boolean,
      },

      hasShadow_: {
        type: Boolean,
        value: false,
        reflectToAttribute: true,
      },

      inSearchMode_: {
        type: Boolean,
        value: false,
      },

      items_: {
        type: Array,
        value() {
          return [];
        },
      },

      spinnerActive_: {
        type: Boolean,
        notify: true,
      },

      lastFocused_: Object,

      listBlurred_: Boolean,
    };
  }

  static get observers() {
    return ['itemsChanged_(items_.*)'];
  }

  private items_: Array<MojomData>;
  private hasDownloads_: boolean;
  private hasShadow_: boolean;
  private inSearchMode_: boolean;
  private spinnerActive_: boolean;

  private mojoHandler_: PageHandlerInterface;
  private mojoEventTarget_: PageCallbackRouter;
  private searchService_: SearchService = SearchService.getInstance();
  private loaded_: PromiseResolver<void> = new PromiseResolver();
  private listenerIds_: Array<number>;
  private eventTracker_: EventTracker = new EventTracker();

  constructor() {
    super();

    const browserProxy = BrowserProxy.getInstance();

    this.mojoEventTarget_ = browserProxy.callbackRouter;

    this.mojoHandler_ = browserProxy.handler;

    // Regular expression that captures the leading slash, the content and the
    // trailing slash in three different groups.
    const CANONICAL_PATH_REGEX = /(^\/)([\/-\w]+)(\/$)/;
    const path = location.pathname.replace(CANONICAL_PATH_REGEX, '$1$2');
    if (path !== '/') {  // There are no subpages in chrome://downloads.
      window.history.replaceState(undefined /* stateObject */, '', '/');
    }
  }

  /** @override */
  connectedCallback() {
    super.connectedCallback();

    // TODO(dbeam): this should use a class instead.
    this.toggleAttribute('loading', true);
    document.documentElement.classList.remove('loading');

    this.listenerIds_ = [
      this.mojoEventTarget_.clearAll.addListener(this.clearAll_.bind(this)),
      this.mojoEventTarget_.insertItems.addListener(
          this.insertItems_.bind(this)),
      this.mojoEventTarget_.removeItem.addListener(this.removeItem_.bind(this)),
      this.mojoEventTarget_.updateItem.addListener(this.updateItem_.bind(this)),
    ];

    this.eventTracker_.add(
        document, 'keydown', e => this.onKeyDown_(e as KeyboardEvent));
    this.eventTracker_.add(document, 'click', () => this.onClick_());

    this.loaded_.promise.then(() => {
      window.requestIdleCallback(function() {
        chrome.send(
            'metricsHandler:recordTime',
            ['Download.ResultsRenderedTime', window.performance.now()]);
        // https://github.com/microsoft/TypeScript/issues/13569
        (document as any).fonts.load('bold 12px Roboto');
      });
    });

    this.searchService_.loadMore();

    // Intercepts clicks on toast.
    const toastManager = getToastManager();
    toastManager.shadowRoot!.querySelector<HTMLElement>('#toast')!.onclick =
        e => this.onToastClicked_(e);
  }

  /** @override */
  disconnectedCallback() {
    super.disconnectedCallback();

    this.listenerIds_.forEach(
        id => assert(this.mojoEventTarget_.removeListener(id)));

    this.eventTracker_.removeAll();
  }

  private clearAll_() {
    this.set('items_', []);
  }

  private hasDownloadsChanged_() {
    if (this.hasDownloads_) {
      this.$.downloadsList.fire('iron-resize');
    }
  }

  private insertItems_(index: number, items: Array<MojomData>) {
    // Insert |items| at the given |index| via Array#splice().
    if (items.length > 0) {
      this.items_.splice(index, 0, ...items);
      this.updateHideDates_(index, index + items.length);
      this.notifySplices('items_', [{
                           index: index,
                           addedCount: items.length,
                           object: this.items_,
                           type: 'splice',
                           removed: [],
                         }]);
    }

    if (this.hasAttribute('loading')) {
      this.removeAttribute('loading');
      this.loaded_.resolve();
    }

    this.spinnerActive_ = false;
  }

  private itemsChanged_() {
    this.hasDownloads_ = this.items_.length > 0;
    this.$.toolbar.hasClearableDownloads =
        loadTimeData.getBoolean('allowDeletingHistory') &&
        this.items_.some(
            ({state}) => state !== States.DANGEROUS &&
                state !== States.MIXED_CONTENT &&
                state !== States.IN_PROGRESS && state !== States.PAUSED);

    if (this.inSearchMode_) {
      this.dispatchEvent(new CustomEvent('iron-announce', {
        bubbles: true,
        composed: true,
        detail: {
          text: this.items_.length === 0 ?
              this.noDownloadsText_() :
              (this.items_.length === 1 ?
                   loadTimeData.getStringF(
                       'searchResultsSingular',
                       this.$.toolbar.getSearchText()) :
                   loadTimeData.getStringF(
                       'searchResultsPlural', this.items_.length,
                       this.$.toolbar.getSearchText()))
        }
      }));
    }
  }

  /**
   * @return The text to show when no download items are showing.
   */
  private noDownloadsText_(): string {
    return loadTimeData.getString(
        this.inSearchMode_ ? 'noSearchResults' : 'noDownloads');
  }

  private onKeyDown_(e: KeyboardEvent) {
    let clearAllKey = 'c';
    // <if expr="is_macosx">
    // On Mac, pressing alt+c produces 'ç' as |event.key|.
    clearAllKey = 'ç';
    // </if>
    if (e.key === clearAllKey && e.altKey && !e.ctrlKey && !e.shiftKey &&
        !e.metaKey) {
      this.onClearAllCommand_();
      e.preventDefault();
      return;
    }

    if (e.key === 'z' && !e.altKey && !e.shiftKey) {
      let hasTriggerModifier = e.ctrlKey && !e.metaKey;
      // <if expr="is_macosx">
      hasTriggerModifier = !e.ctrlKey && e.metaKey;
      // </if>
      if (hasTriggerModifier) {
        this.onUndoCommand_();
        e.preventDefault();
      }
    }
  }

  private onClick_() {
    const toastManager = getToastManager();
    if (toastManager.isToastOpen) {
      toastManager.hide();
    }
  }

  private onClearAllCommand_() {
    if (!this.$.toolbar.canClearAll()) {
      return;
    }

    this.mojoHandler_.clearAll();
    const canUndo =
        this.items_.some(data => !data.isDangerous && !data.isMixedContent);
    getToastManager().show(
        loadTimeData.getString('toastClearedAll'),
        /* hideSlotted= */ !canUndo);
  }

  private onUndoCommand_() {
    if (!this.$.toolbar.canUndo()) {
      return;
    }

    getToastManager().hide();
    this.mojoHandler_.undo();
  }

  private onToastClicked_(e: Event) {
    e.stopPropagation();
    e.preventDefault();
  }

  private onScroll_() {
    const container = this.$.downloadsList.scrollTarget!;
    const distanceToBottom =
        container.scrollHeight - container.scrollTop - container.offsetHeight;
    if (distanceToBottom <= 100) {
      // Approaching the end of the scrollback. Attempt to load more items.
      this.searchService_.loadMore();
    }
    this.hasShadow_ = container.scrollTop > 0;
  }

  private onSearchChanged_() {
    this.inSearchMode_ = this.searchService_.isSearching();
  }

  private removeItem_(index: number) {
    const removed = this.items_.splice(index, 1);
    this.updateHideDates_(index, index);
    this.notifySplices('items_', [{
                         index: index,
                         addedCount: 0,
                         object: this.items_,
                         type: 'splice',
                         removed: removed,
                       }]);
    this.onScroll_();
  }

  private onUndoClick_() {
    getToastManager().hide();
    this.mojoHandler_.undo();
  }

  /**
   * Updates whether dates should show for |this.items_[start - end]|. Note:
   * this method does not trigger template bindings. Use notifySplices() or
   * after calling this method to ensure items are redrawn.
   */
  private updateHideDates_(start: number, end: number) {
    for (let i = start; i <= end; ++i) {
      const current = this.items_[i];
      if (!current) {
        continue;
      }
      const prev = this.items_[i - 1];
      current.hideDate = !!prev && prev.dateString === current.dateString;
    }
  }

  private updateItem_(index: number, data: MojomData) {
    this.items_[index] = data;
    this.updateHideDates_(index, index);

    this.notifyPath(`items_.${index}`);
    setTimeout(() => {
      const list = this.$.downloadsList;
      list.updateSizeForIndex(index);
    }, 0);
  }

  // Override FindShortcutBehavior methods.
  handleFindShortcut(modalContextOpen: boolean): boolean {
    if (modalContextOpen) {
      return false;
    }
    this.$.toolbar.focusOnSearchInput();
    return true;
  }

  // Override FindShortcutBehavior methods.
  searchInputHasFocus() {
    return this.$.toolbar.isSearchFocused();
  }

  static get template() {
    return html`{__html_template__}`;
  }
}

customElements.define(DownloadsManagerElement.is, DownloadsManagerElement);
