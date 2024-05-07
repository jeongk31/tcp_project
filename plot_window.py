import matplotlib.pyplot as plt

x = []
y1 = []
y2 = []

with open("CWND.csv", "r") as fp:
    fp.readline()
    for line in fp:
        data = line.strip().split(",")
        x.append(float(data[0]))
        y1.append(float(data[1]))
        y2.append(int(data[2]))

fig, ax1 = plt.subplots()
ax2 = ax1.twinx()

# plot first set of data
ax1.plot(x, y1, "g-")
ax1.set_ylim([0, max(y1)])
#ax1.set_ylim([0, 100])
# plot second set of data
ax2.step(x, y2, "r--")
ax2.set_ylim([0, max(y1)])
#ax2.set_xlim([0,5])

ax1.set_ylabel("Window Size (num. packets)")
ax1.set_xlabel("Time (seconds)")
ax2.set_ylabel("ss_thresh (num. packets)")

    # set colours for each y-axis to tell them apart
ax1.yaxis.label.set_color("g")
ax2.yaxis.label.set_color("r")

plt.savefig("cwnd.pdf")
plt.show()
