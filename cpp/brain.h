#pragma once
/* ================= 大脑通道（异步）=================
   脊髓不调 LLM。它只在"心里没数"的时候挂一块牌子：
        brain.pending = 1, tag = n
   然后**继续跑**，用旧意图顶着。大脑什么时候回来都行；
   回来就带同一个 tag，脊髓才认。tag 不对 = 丢弃（迟到的回答不许乱改现在）。
*/
struct BrainChannel {
  int nextTag = 1;
  int pendingTag = 0;        // 0 = 没有在等的
  /* 4 s：大脑是个真模型，一次往返 0.5–3 s 都正常。
     0.6 s 那档只够本地小函数，会把每次回答都判成迟到。 */
  double askedAt = 0, waitMax = 4.0;   // 等 4 s 还没回就自己拿主意
  int answered = 0, dropped = 0, timedOut = 0;

  bool pending() const { return pendingTag != 0; }

  /* 脊髓：请求大脑（同一个 tag 不会重复挂） */
  int ask() {
    if (pendingTag) return pendingTag;
    pendingTag = nextTag++;
    askedAt = 0;            /* 新牌子，重新计时 */
    return pendingTag;
  }

  /* 大脑回来了：tag 必须等于在等的那个 */
  bool answer(int tag) {
    if (!pendingTag) { ++dropped; return false; }
    if (tag != pendingTag) { ++dropped; return false; }
    pendingTag = 0; ++answered;
    return true;
  }

  /* 超时：脊髓自己决定，不等了 */
  bool tick(double dt) {
    if (!pendingTag) return false;
    askedAt += dt;
    if (askedAt >= waitMax) { askedAt = 0; pendingTag = 0; ++timedOut; return true; }
    return false;
  }
};
